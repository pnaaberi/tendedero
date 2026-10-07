# Pegline Linux verification — 2026-10-07

Built and installed on the verified KDE desktop: CachyOS, KDE Plasma Wayland, Qt 6.11.2,
Rust 1.97.1, one 1920×1080 screen. The local executable is
`~/.local/bin/pegline`; `pegline.service` is enabled and running.

## Evidence

- 11 Rust tests and one native Qt integration test passed. The tests use
  real filesystem operations, image data, card events, and nested menu events.
- Formatting, Clippy with warnings denied, the locked release build, shell
  syntax, desktop-file validation, and installed user-service validation passed.
- The real Wayland clipboard failure check passed: an unavailable connection
  returns failure and supplies an error, without displaying success.
- The installed live KDE check passed: screenshot ingestion, complete PNG
  transfer larger than 64 KiB, file URL transfer, save-copy byte preservation,
  file-retaining dismissal, restart persistence, active global shortcut and
  shortcut delivery, and reveal. All generated watched/Pictures fixtures were
  removed. Desktop screenshots stayed in private temporary directories.
- The installed executable matched the release executable's SHA-256:
  `37c4810962eb9679b446b28fc5f4ca98ac762e14e451351f706d321b4d8346c5`.

## Review and corrections

A fresh reviewer examined the whole port from upstream commit `3c866d9`
through `db0a833`. One fix pass addressed four findings:

- Panel retirement now closes menus and cancels drags, then releases the
  panel after nested handlers return. A tracked handler regression failed
  before the fix and passed afterward; restoring unconditional deletion
  also made the regression fail.
- Native Wayland clipboard failure no longer reports success. The live
  failure regression was observed failing before the fix and passing after it.
- Transient scan errors clear on recovery, permitting repeat notifications
  after a later failure. Corrupt-state recovery notices remain separate.
- Repeated failures decoding a stable image report its path, while preserving
  retries and avoiding notifications for brief incomplete writes.

The last two findings were treated as material because misleading or missing
failure feedback violates the intended user behavior. Both filesystem
regressions failed first and passed after correction. No review findings
remain deferred.

## Decisions and supported limits

1. `Store::scan` accepts a decoder callback, reusing installed Qt codecs rather
   than adding another image library. If unsuitable, one method signature and
   its caller would need changing.
2. A small native `ext-data-control-v1` adapter provides background clipboard
   access: this KWin session advertises the newer protocol, while installed
   Qt does not use it. The cost is the adapter and Wayland development files.
3. Spectacle replaces macOS capture/annotation APIs; fullscreen stacking
   follows KDE's top-layer policy. Capture-flight effects and macOS fullscreen
   parity are outside the approved scope. Changing this requires a separate
   animation or stacking feature.
4. KDE Wayland is the verified target. Other compositors and multiple seats
   are unverified, and live multi-screen testing needs additional hardware.
   Expanding support requires compositor/hardware checks and possibly protocol
   adjustments.

The port uses the independent name **Pegline** and its own icon to respect
the upstream project's restricted name/artwork. Original source history and
MIT attribution remain in the fork.
