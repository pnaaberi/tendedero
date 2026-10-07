# Build and contribute to Pegline

[Back to the README](../README.md) ·
[Verification record](superpowers/pegline-linux-verification.md)

## Requirements

The Linux app lives in `linux/`. The root Swift package and `scripts/` are the
preserved macOS implementation and do not build the Linux app.

Use a current stable Rust toolchain (Rust 1.97.1 was verified), a C++17 compiler,
CMake 3.20 or newer, Ninja, pkg-config, Qt 6.5 or newer with Widgets and D-Bus, LayerShellQt
6.6 or newer, KF6GlobalAccel, Wayland development files, and `wayland-scanner`.
Native smoke tests additionally use Qt Test. Spectacle supplies capture and
annotation at runtime; Dolphin supplies **Show in folder**.

The [README installation command](../README.md#install) names the CachyOS/Arch
packages. See the official Arch package records for
[LayerShellQt](https://archlinux.org/packages/extra/x86_64/layer-shell-qt/),
[KGlobalAccel](https://archlinux.org/packages/extra/x86_64/kglobalaccel/), and
[Qt tools](https://archlinux.org/packages/extra/x86_64/qt6-tools/).
Other distributions need equivalent development packages and a compatible
desktop session; they have not received equivalent live validation.

## Build and run

Run from the repository root:

```sh
cargo build --manifest-path linux/Cargo.toml --release --locked
linux/target/release/pegline --help
```

To test the built executable instead of the installed service, stop the
service first and use a dedicated screenshot directory:

```sh
systemctl --user stop pegline.service
linux/target/release/pegline --watch "$HOME/Pictures/Pegline-Test" --daemon
```

The foreground process runs until Ctrl+C. Restore the installed service with
`systemctl --user start pegline.service`. Watch overrides do not redirect
Spectacle's output; save a synthetic image into the test directory or match
Spectacle's configured save location.

## Automated checks

```sh
cargo fmt --manifest-path linux/Cargo.toml --check
cargo test --manifest-path linux/Cargo.toml --locked
cargo clippy --manifest-path linux/Cargo.toml --locked --all-targets -- -D warnings
cargo build --manifest-path linux/Cargo.toml --release --locked
```

`cargo test` runs the Rust filesystem tests and the native Qt checks. It needs
the native development dependencies even without a running desktop. Qt UI
tests use the offscreen platform and temporary real PNGs. To run only those
checks:

```sh
bash linux/scripts/check-ui.sh
```

The native suite covers rendering, input regions, full image clipboard data,
cancelled drag ownership, file-retaining dismissal, retirement during a nested
menu, flight trajectory and landing, the full ten-second preview, timer reset,
early hide, and quiet restoration of historical images. The arrival check
takes about thirteen seconds because it exercises real timers.

## Desktop checks

Offscreen rendering cannot establish compositor behavior or hardware shortcut
delivery. Run these checks deliberately in a KDE Wayland session with the app
installed. On Arch, the live script's extra tools are:

```sh
sudo pacman -S --needed python wl-clipboard qt6-tools
```

After the native suite builds its smoke executable, test failure handling on
the real Wayland platform:

```sh
QT_QPA_PLATFORM=wayland linux/target/ui-check/pegline-ui-smoke --clipboard-failure
python3 linux/scripts/check-live.py
```

The live script requires at most five existing cards so its three fixtures do
not evict user history. It verifies ingestion, full PNG transfer larger than
64 KiB, file-URL transfer, safe copying, dismissal, restart persistence, and
KDE toggle delivery. It **changes the clipboard, restarts Pegline, and saves a
desktop screenshot** in a local temporary directory. It removes only its
watched and Pictures fixtures afterward. Keep the evidence private and remove
it when no longer needed.

That script invokes the KDE toggle action through D-Bus; it does not press a
physical key or verify the region-selection UI. For capture changes, also
check Meta+Shift+S, selection and cancellation, flight into the line, ten-second
hide, repeated-capture reset, manual hide, and click-through on every connected
display. Use a synthetic test window rather than personal desktop content.

## Architecture

| File | Responsibility |
| --- | --- |
| `linux/src/store.rs` | Stable-file scanning, startup baseline, eight-card history, persistence, safe copies |
| `linux/src/main.rs` | CLI, XDG paths, JSON snapshots, and C ABI ownership |
| `linux/ui/pegline.cpp` | Tray, Spectacle processes, KDE shortcuts, per-screen panels, session D-Bus controls |
| `linux/ui/panel.cpp` | Painting, arrival and reveal timers, input mask, card gestures, drag/drop |
| `linux/ui/clipboard.cpp` | Background PNG and file-URL transfer through Wayland data-control |
| `linux/build.rs`, `linux/ui/CMakeLists.txt` | Native compilation and linkage from Cargo |
| `linux/data/`, `linux/scripts/` | Icon, desktop entry, user service, installation, and checks |

Rust owns the store and allocated JSON strings; Qt owns the event loop and
desktop interactions. The C ABI pairs returned strings with
`pg_string_free`. Store callbacks run on the GUI thread. A 500 ms timer samples
the watch folder; a file must remain unchanged across two samples and decode
successfully before it is added. First-scan fingerprints distinguish startup
imports from captures made while the app runs.

Each screen shares the same card list. During the 800 ms flight its panel
temporarily expands to the screen height, while the moving preview passes
input through. After landing the panel shrinks to its normal height. The
ten-second capture timer survives pointer leave and defers during active
card interactions; hiding cancels the flight and timer.

## Changes and publication

Keep changes focused and include checks that exercise the affected behavior.
Update the user-facing README or troubleshooting guide when commands, startup,
shortcuts, storage, or interactions change. Keep
[the verification record](superpowers/pegline-linux-verification.md) aligned
with completed checks and distinguish automated coverage from live evidence.

Before a public push or release, audit the exact outgoing commits and files
for secrets, private metadata, home paths, screenshots, logs, runtime data,
dependency advisories, and attribution. Include newly reachable history;
editing the current tree does not remove a secret from an earlier commit.
Keep raw audit reports outside the repository and redact sensitive output.
Do not publish desktop test evidence or local history files.

Retain the [MIT attribution](../LICENSE), the vendored protocol notices, and
the independent Pegline name and icon. The upstream Tendedero name, icon,
and documentation artwork are restricted separately from its source code.
