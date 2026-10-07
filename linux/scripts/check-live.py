#!/usr/bin/env python3
"""Check an installed Pegline on the active KDE session; remove only own fixtures."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import zlib


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def status():
    return json.loads(run("pegline", "--status"))


def wait_for(predicate):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        try:
            current = status()
            if predicate(current):
                return current
        except subprocess.CalledProcessError:
            pass
        time.sleep(0.2)
    raise AssertionError("Pegline did not reach the expected state")


def png(width, height, color):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = (b"\0" + bytes(color) * width) * height
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


def action(operation, path):
    return json.loads(run("qdbus6", "org.choppy.Pegline", "/Pegline", "org.choppy.Pegline.Action", operation, str(path)))


def main():
    before = status()
    assert before["platform"] == "wayland", before
    assert len(before["items"]) <= 5, "Live test needs room for three fixtures without evicting user history"
    component = "/component/org_choppy_Pegline"
    assert run("qdbus6", "org.kde.kglobalaccel", component, "org.kde.kglobalaccel.Component.isActive") == "true", "Pegline's global shortcut must stay active after restart"
    watched = Path(before["watch"])
    fixtures = []
    saved = None
    evidence = Path(tempfile.mkdtemp(prefix="pegline-live-"))
    try:
        for n, (size, color) in enumerate([((480, 300), (70, 123, 180)), ((280, 400), (104, 165, 128)), ((600, 250), (190, 139, 88))]):
            path = watched / f"pegline-check-{os.getpid()}-{n}.png"
            path.write_bytes(png(*size, color))
            fixtures.append(path)
        paths = {str(path) for path in fixtures}
        wait_for(lambda s: paths <= {item["path"] for item in s["items"]})
        assert action("copy", fixtures[0])["ok"]
        assert "image/png" in run("wl-paste", "--list-types")
        assert str(fixtures[0]).replace(" ", "%20") in run("wl-paste", "--type", "text/uri-list")
        clipboard = subprocess.check_output(["wl-paste", "--type", "image/png"])
        assert struct.unpack(">II", clipboard[16:24]) == (480, 300), "clipboard must contain full-size image"
        result = action("save", fixtures[0])
        assert result["ok"], result
        saved = Path(result["saved"])
        assert saved.read_bytes() == fixtures[0].read_bytes()
        assert action("dismiss", fixtures[2])["ok"]
        assert fixtures[2].exists(), "dismissal deleted source"
        subprocess.run(["systemctl", "--user", "restart", "pegline.service"], check=True)
        expected = {str(path) for path in fixtures[:2]}
        wait_for(lambda s: expected <= {item["path"] for item in s["items"]} and str(fixtures[2]) not in {item["path"] for item in s["items"]})
        assert run("qdbus6", "org.kde.kglobalaccel", component, "org.kde.kglobalaccel.Component.isActive") == "true"
        visible = status()["visible"]
        run("qdbus6", "org.kde.kglobalaccel", component, "org.kde.kglobalaccel.Component.invokeShortcut", "ToggleLine")
        wait_for(lambda s: s["visible"] != visible)
        run("qdbus6", "org.kde.kglobalaccel", component, "org.kde.kglobalaccel.Component.invokeShortcut", "ToggleLine")
        wait_for(lambda s: s["visible"] == visible)
        run("pegline", "--show")
        wait_for(lambda s: s["visible"])
        subprocess.run(["spectacle", "--background", "--nonotify", "--fullscreen", "--delay", "400", "--output", str(evidence / "desktop.png")], check=True)
        print(f"Live KDE check passed: ingestion, PNG clipboard, safe copy, dismissal, restart persistence, shortcut, reveal. Evidence: {evidence / 'desktop.png'}")
    finally:
        for path in fixtures:
            path.unlink(missing_ok=True)
        if saved:
            saved.unlink(missing_ok=True)
        run("pegline", "--hide")
        wait_for(lambda s: not any(item["path"] in {str(path) for path in fixtures} for item in s["items"]))


if __name__ == "__main__":
    main()
