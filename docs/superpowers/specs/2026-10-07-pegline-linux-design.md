# Pegline: native screenshot line for KDE Wayland

Updated 2026-10-07 to include the shipped capture-and-reveal follow-up.
See the [user guide](../../../README.md),
[development guide](../../development.md), and
[verification record](../pegline-linux-verification.md) for current commands
and completed checks. The companion plan preserves the original task sequence.

## Intent and success criteria

Port the useful behavior of Alejandro Buján's Tendedero to a
CachyOS/KDE Wayland desktop, using Rust where it provides value. Deliver an
installed, running application and a GitHub fork that can be rebuilt. The
original uses AppKit and SwiftUI, which cannot run on Linux.

The modified app is called **Pegline**, with its own simple line-and-peg icon.
Keep the upstream MIT attribution and source history. Do not reuse the
restricted upstream name as the app name, icon artwork, or README images.

## Selected approach

Use a Rust executable for screenshot discovery, item state, persistence, and
safe save operations. Link a small C++ Qt6 Widgets interface through a narrow
C ABI. Qt6, LayerShellQt, and KF6GlobalAccel are available on the target KDE desktop.
This gives native Wayland positioning, drag-and-drop, image clipboard support,
tray menus, and global shortcuts without installing another desktop toolkit.

Alternatives considered: all Rust with GTK4 would require another layer-shell
library and fit KDE less closely; a complete C++/Qt port is simpler but omits
the user's preferred Rust core. A web wrapper adds an unnecessary runtime.

## Behavior

- Watch Spectacle's configured image directory, using its settings rather than hardcoding
  the user's home directory. Allow `--watch DIRECTORY` as an explicit override.
- New complete images appear on a gently sagging line at the top of the screen.
  Accept PNG, JPEG, WebP, BMP, GIF, and TIFF when Qt can decode them. Ignore
  videos, hidden files, directories, symlinks, and incomplete image writes.
- Show up to eight recent screenshots. Prune moved or deleted files. Keep
  state across restarts; dismissal or capacity eviction never deletes files.
- A small hover target at the top center reveals the line. Leaving hides it
  after a short delay for manual reveals. New captures reveal the line for ten
  seconds, reset that timer on another capture, and survive pointer leave.
  Active card presses, drags, and menus postpone automatic hiding. Manual hide
  cancels the timer and flight. Empty areas pass clicks through; the app does
  not take keyboard focus. Use the compositor's top layer, with no reserved
  screen area.
- The tray menu and `Meta+Alt+T` show or hide it. Register the shortcut through
  KDE without taking an existing shortcut away. Support `--show`, `--hide`,
  `--toggle`, `--capture-region`, `--quit`, and `--status` commands against the
  running instance. Meta+Shift+S captures a region through Spectacle; a
  conflicting KDE assignment must be changed explicitly by the user.
- Click a card to copy its image and file URL, double-click to open it with the
  normal image viewer, and hold for 450 ms to open Spectacle's existing-image
  annotation editor. Right-click exposes the same actions and removal.
- Drag a card into an app or folder as a real file. A copy keeps it on the
  line. A completed move removes it only when the source file is gone. Never
  delete a source on an ambiguous drag result.
- The card's cross takes it down while retaining the original file. Saving a
  copy to Pictures uses a unique filename and never overwrites an existing file.
- Start with one surface per screen, sharing the screenshot list, and update
  surfaces when outputs change. Capture arrival has been validated on two
  displays with mixed scaling.
- Provide screenshot capture actions through Spectacle, with Pegline hidden
  while capture starts. An owned process prevents overlapping capture requests
  and reports launch/completion failures. Escape cancels region selection.
  Existing Print Screen shortcuts continue to work.
- A completed live capture flies from screen center into its card over 800 ms.
  The temporary expanded surface passes preview input through and shrinks
  after landing. Files present at startup restore without flight or reveal.
- Install to the user's local bin and application menu, with a user service
  that starts with the graphical session. No audio configuration changes.

## Architecture and files

- `linux/Cargo.toml`, `linux/build.rs`: Rust package and native Qt compilation.
- `linux/src/store.rs`: folder discovery, stable-file scanning, eight-item
  history, atomic state persistence, and collision-safe copy to Pictures.
- `linux/src/main.rs`: CLI, C ABI ownership, and calls into the Qt event loop.
- `linux/ui/pegline.cpp`: per-screen panels, Spectacle processes, tray, KDE
  shortcuts, and a session D-Bus control endpoint.
- `linux/ui/panel.cpp`: painting, flight/reveal timers, input regions, card
  gestures, menus, clipboard actions, and drag/drop.
- `linux/ui/clipboard.cpp`: native background Wayland clipboard transfers.
- `linux/data/`: independent SVG icon, desktop entry, and user service.
- `linux/scripts/install.sh`: reproducible build/install, without root access.
- Root README: Linux usage/build guide and upstream attribution. Preserve the
  upstream macOS implementation and build scripts as reference material.

The C ABI passes owned UTF-8 JSON snapshots and action results. Rust allocates
and frees these strings explicitly. Callbacks run on the Qt GUI thread, so
state needs no shared locks or background task runtime. A 500 ms timer samples
file metadata; a file must remain stable across samples before decode/add.
This polling tradeoff is acceptable for a small dedicated screenshot folder.
The first successful scan also records a fingerprint baseline. Snapshots mark
accepted files whose fingerprints differ from that baseline as new captures;
this prevents delayed historical imports from triggering the arrival effect.

## Data and failure handling

Use XDG state/config directories, defaulting to their conventional locations.
The application never moves or removes watched originals on dismissal,
overflow, or exit. Store only paths, timestamps, and dismissed fingerprints;
images stay in the screenshot folder. Persist through a temporary file and
atomic rename. Report permission, decode, clipboard, copy, and command failures
through visible feedback and stderr, without claiming that an action succeeded.
Missing watch directories can be created; unavailable folders are reported and
retried. Treat a failed scan differently from a successfully scanned empty
folder, so an access error does not erase history.

No network access or telemetry is needed at runtime. GitHub is used only to
create/publish the requested fork. Single-instance control uses session D-Bus.

## Verification

Rust tests use temporary directories and real file operations: stable file
discovery, image filtering, capacity eviction, dismissed-file persistence,
missing-file pruning, settings paths containing spaces, unreadable/corrupt
state handling, and copy collision behavior. Native UI smoke tests exercise
the actual Qt clipboard, card events, hover/reveal state, and rendering.

Run formatting, the full Rust suite, Clippy, a release build, and a live KDE
Wayland check. Verify the tray registration, D-Bus status, shortcut registration,
new screenshot ingestion, copy/paste image MIME, restart persistence, and a
desktop screenshot of the revealed line. Keep generated test images outside
the normal screenshot folder except temporary fixtures that are removed.

## Deliberate Linux differences

Pegline uses native KDE desktop APIs. Its flight starts at screen center because
Spectacle exports do not reliably include selection coordinates. There is no
Apple Markup integration. Annotation uses Spectacle. Dismissal retains files
rather than taking over the screenshot
destination and implicitly trashing captures. Fullscreen stacking follows the
Wayland compositor's top-layer policy; do not claim macOS fullscreen parity
without checking that behavior on KDE.
