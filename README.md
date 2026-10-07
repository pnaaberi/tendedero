# Pegline

Screenshots, hanging within reach. A native **KDE Wayland** port of
[Alejandro Buján's Tendedero](https://github.com/alejandrobujan/tendedero), with a
Rust core and a small Qt6 interface.

Move the pointer to the **top center of your screen** or press **Meta+Alt+T**.
Recent screenshots hang from a line; move away and it tucks back out of sight.
The app lives in your system tray and starts with your graphical session.

| Gesture | Action |
| --- | --- |
| Click a screenshot | Copy the full image and file URL |
| Double-click | Open in your usual image viewer |
| Hold for 450 ms | Annotate in Spectacle |
| Drag into an app or folder | Share a real file; file managers handle moves |
| Right-click | Copy, open, annotate, show in folder, save a copy, or take down |
| Click the cross | Take down the card and keep its file |
| Meta+Alt+T / tray icon | Show or hide the line |

Pegline reads Spectacle's screenshot folder from `spectaclerc`, watches for
complete image files, and remembers up to eight recent captures across
restarts. It ignores video recordings. Dismissing cards, exceeding the limit,
and quitting **never delete your screenshots**. Saving to Pictures creates a
unique filename and never overwrites an existing file.

Use your existing Print Screen shortcuts. The tray also offers region and
current-screen captures. Pegline does not change your Spectacle preferences.
No account, network access, or telemetry is used at runtime.

## Install on CachyOS / Arch KDE

Tested with CachyOS, KDE Wayland, Rust 1.97.1, and Qt 6.11.2.
Qt6, KDE LayerShellQt, KF6GlobalAccel, Wayland development files, Spectacle, CMake, Ninja, and a C++ compiler
are required. If those development packages are missing:

```sh
sudo pacman -S --needed rust base-devel cmake ninja qt6-base qt6-wayland wayland layer-shell-qt kglobalaccel spectacle
```

```sh
git clone --branch linux-pegline https://github.com/pnaaberi/tendedero.git
cd tendedero
bash linux/scripts/install.sh
```

The installer builds a release executable and installs `~/.local/bin/pegline`,
an application-menu entry, its own icon, and a systemd user service. No root
access is used by the installer. `~/.local/bin` must be on your desktop's PATH.
Use `--no-start` to install without enabling or starting the service.

## Commands

```sh
pegline --show
pegline --hide
pegline --toggle
pegline --status
pegline --quit
```

To watch another screenshot folder, stop the service and launch with an override:

```sh
systemctl --user stop pegline
pegline --watch "$HOME/Pictures/Screenshots"
```

Use a systemd service override to make an alternate folder permanent. Set the
KDE shortcut in System Settings → Keyboard → Shortcuts → Pegline. Pegline
leaves an already assigned shortcut alone; its tray icon always remains usable.

History is kept at `${XDG_STATE_HOME:-~/.local/state}/pegline/state.json`.
Screenshot files stay in their original folder. A corrupt history file is
preserved beside the state file for recovery.

```sh
systemctl --user status pegline
journalctl --user -u pegline -n 30
bash linux/scripts/uninstall.sh
```

Uninstalling retains screenshots and history.

## Build and verify

```sh
cargo build --manifest-path linux/Cargo.toml --release --locked
cargo test --manifest-path linux/Cargo.toml
cargo fmt --manifest-path linux/Cargo.toml --check
cargo clippy --manifest-path linux/Cargo.toml --all-targets -- -D warnings
```

The test suite includes real filesystem tests and a native Qt smoke test for
rendering, input regions, image clipboard content, file retention, scan-error
recovery, and panel lifetime during a nested menu. UI
checks run using Qt's offscreen platform. `bash linux/scripts/check-ui.sh`
runs just the native checks. Check the live Wayland desktop as well before
shipping compositor-related changes.

On the live Wayland session, `QT_QPA_PLATFORM=wayland linux/target/ui-check/pegline-ui-smoke --clipboard-failure`
also checks that an unavailable clipboard connection reports failure.

On an installed KDE session, `python linux/scripts/check-live.py` verifies
real screenshot ingestion, complete large PNG and file-URL clipboard transfers, safe copying,
dismissal, service restart, and shortcut delivery. It requires `wl-clipboard`
and room for three temporary cards; it removes its fixtures afterward and
keeps desktop evidence in a private temporary folder. This check restarts
Pegline and changes the clipboard, so run it deliberately.

`linux/src/store.rs` owns discovery, stability checks, history, and safe copy
operations. `linux/src/main.rs` supplies the C ABI and command-line options.
`linux/ui/panel.cpp` paints the line and handles gestures; `pegline.cpp` provides
the tray, KDE shortcut, and session D-Bus controls. `clipboard.cpp` supports
KDE's newer data-control protocol without requesting keyboard focus.
Cargo invokes CMake to link
the native interface. The runtime uses only Qt/KDE libraries already present
on a KDE desktop.

## Scope and attribution

This port targets KDE Plasma 6 on Wayland. Other Wayland compositors need the
layer-shell protocol and a compatible tray; they are not verified here. There
is a basic Qt/X11 fallback, but KDE Wayland is the tested target. Multi-screen
surfaces are implemented; the live check uses a single screen.

Fullscreen stacking follows the compositor's top-layer policy. Apple Markup
is replaced by Spectacle, and macOS capture-flight effects are not ported.
Screenshot-directory takeover and automatic trashing are deliberately omitted.

The original macOS sources and scripts remain in `Sources/Tendedero` and
`scripts` as reference. They still require macOS; build the Linux app from
`linux/`. Pegline uses a different app name and original icon because the
upstream name and icon are excluded from its license. Code remains MIT
licensed; see [LICENSE](LICENSE) for Alejandro Buján's original attribution.
Pegline is an independent fork and is not endorsed by the upstream author.
The vendored Wayland protocol definition carries its own permissive license
and copyright notices in `linux/ui/ext-data-control-v1.xml`.
