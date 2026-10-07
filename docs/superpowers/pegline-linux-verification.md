# Pegline Linux verification

Last verified: **2026-10-07**. Application source:
[`56aec8d`](https://github.com/pnaaberi/tendedero/commit/56aec8d32e549ea70e13ed4b0cc6a4ff9735474f).
This record covers the Rust/Qt port and the shipped region-capture, screenshot
flight, and ten-second reveal update. It records completed checks, not a
promise of support for every desktop configuration.

[User guide](../../README.md) · [Troubleshooting](../troubleshooting.md) ·
[Build and checks](../development.md)

## Automated evidence

- **12 Rust unit tests** and **one native Qt integration test** passed. The
  integration test runs both the normal UI smoke mode and the arrival mode.
- The filesystem suite covers stable writes and decode retries, filtering,
  eight-card capacity, dismissal persistence, removed files, scan recovery,
  corrupt-state preservation, settings parsing, copy collisions, and the
  distinction between historical images and captures made after startup.
- Native checks cover rendered cards, input regions, image clipboard data,
  cancelled drag ownership, file-retaining dismissal, and safe retirement
  during nested menu handlers.
- Arrival checks inspect rendered frames to establish upward travel, preview
  click-through, landing, restored card input, the full ten-second timer,
  pointer-leave behavior, repeated-capture reset, manual hide, and retained
  source files. Historical imports do not start the flight.
- Cargo formatting, Clippy with warnings denied, and the locked release build
  passed. Desktop-entry, shell-syntax, and installed user-service validation
  also passed during port installation.
- An isolated full-executable check verified that cold-start and offline
  images restore quietly while a new live image reveals the line.

## Live KDE evidence

Verified on CachyOS with KDE Plasma Wayland, Rust 1.97.1, Qt 6.11.2,
LayerShellQt 6.7.5, and Spectacle 6.7.5. Capture arrival was checked on two
connected displays with mixed scaling.

- Actual Meta+Shift+S input launched region selection. Selecting and releasing
  produced a real PNG of a synthetic test window.
- The preview travelled into the line on both displays, landed, and released
  the expanded animation surface. The line remained revealed for the full
  preview and hid automatically afterward. Escape cancellation also passed.
- KDE assigned Meta+Shift+S to Pegline's `CaptureRegion` action while retaining
  Print Screen for Spectacle. Shortcut assignments survived service restart.
- Background clipboard transfer provided a complete full-resolution PNG
  larger than 64 KiB and a file URL. Safe copies preserved bytes, dismissal
  kept the source, and history survived restarting the service.
- The Wayland clipboard failure check reported failure without claiming
  success when a connection was unavailable.
- `pegline.service` was active and enabled for the graphical session. The
  installed and running executable matched the verified release build.
- Only generated fixtures were removed. User screenshots and history were
  retained. Temporary virtual input devices and test windows were removed.
  Desktop evidence and raw audit reports stayed outside the repository.

The checked-in live script exercises ingestion, clipboard, file actions,
persistence, and D-Bus delivery of the toggle action. The region shortcut and
flight checks above were separate live checks; do not treat a script pass as
proof of physical shortcut delivery. See the
[desktop checklist](../development.md#desktop-checks).

## Review corrections

Whole-port review produced fixes for panel lifetime during nested Qt handlers,
false clipboard success, scan-error recovery, and stable-image decode failure
feedback. A separate review of the capture update found that delayed startup
imports could masquerade as new captures. The watcher now carries a startup
fingerprint baseline through the JSON snapshot; the UI animates only live
arrivals. Regression checks failed before the corresponding fixes and passed
afterward. The capture review's Markdown table issue was also corrected.

## Supported limits

- KDE Wayland is the verified target. Other compositors, X11, multiple seats,
  and a broader range of hardware have not received equivalent live testing.
- Fullscreen stacking follows KDE's top-layer policy; macOS fullscreen parity
  has not been established.
- The flight begins at screen center because saved Spectacle images do not
  reliably carry the selected rectangle's coordinates.
- Capture and annotation use Spectacle. A custom `--watch` folder does not
  redirect Spectacle's save location.
- Background Wayland clipboard access requires `ext-data-control-v1`.
  Native system libraries are dynamically linked and are not bundled here.

## Publication and privacy

The public fork is [pnaaberi/tendedero](https://github.com/pnaaberi/tendedero),
with `linux-pegline` as its default branch and the original source history and
MIT attribution retained. Pegline uses an independent name and icon.

The cleaned application history was audited before publication: ten reachable
commits, zero Gitleaks secret findings, and no private desktop captures, logs,
local home paths, or device identifiers in the outgoing history. All eleven
locked registry dependency versions were queried with OSV; no matching known
advisories were found at that time. The query did not cover native system
library advisories. Dependency and secret results are date-specific, so repeat
these checks for later publications.

Earlier documentation references to the local machine were removed across the
published branch history. GitHub can retain old commit objects, and existing
clones or caches can retain prior content; a history rewrite does not erase
those copies. Raw audit reports remain local. New public changes must follow
the [publication checks](../development.md#changes-and-publication).
