# Pegline

**Capture a screenshot. Watch it land. Keep it within reach.**

Pegline is a native screenshot shelf for **KDE Plasma 6 on Wayland**, with a
Rust core and a Qt6 desktop interface. It is an independent Linux port of
[Alejandro Buján's Tendedero](https://github.com/alejandrobujan/tendedero).

Press **Meta+Shift+S**, drag to select a region, and release. The screenshot
flies from screen center onto the line, which opens for **10 seconds**.
Another capture resets the timer. **Meta+Alt+T** hides it early.

Pegline keeps eight recent images, starts when you sign in to KDE, and leaves
your screenshot files in place when you dismiss cards or close the app.

[Install](#install) · [Use Pegline](#use-pegline) ·
[Troubleshooting](docs/troubleshooting.md) ·
[Build and contribute](docs/development.md) ·
[Verification](docs/superpowers/pegline-linux-verification.md)

## Install

Use an up-to-date CachyOS or Arch installation with a KDE Plasma 6 **Wayland**
session. The installer builds from source; there is no prebuilt Linux release.
On an up-to-date system, install the build and desktop dependencies:

```sh
sudo pacman -S --needed git rust base-devel cmake ninja qt6-base qt6-wayland wayland layer-shell-qt kglobalaccel spectacle dolphin
```

Then build, install, and start Pegline as your normal user:

```sh
git clone --branch linux-pegline https://github.com/pnaaberi/tendedero.git
cd tendedero
bash linux/scripts/install.sh
```

The installer adds `~/.local/bin/pegline`, an application-menu entry, an icon,
and `pegline.service`. It enables the service for future KDE logins and starts
it now. Screenshot files and saved history are retained when reinstalling.
Only the package-install command needs administrator access.

Verify that the service is active and enabled:

```sh
systemctl --user is-active pegline.service
systemctl --user is-enabled pegline.service
```

Look for **Pegline** in the application launcher or system tray. If `pegline`
is not found in a terminal, use `~/.local/bin/pegline`; see
[PATH setup](docs/troubleshooting.md#pegline-command-not-found).

### Set the capture shortcut

Open **System Settings → Keyboard → Shortcuts → Pegline** and assign
**Meta+Shift+S** to **Capture a region to Pegline**. If Spectacle already owns
that combination, remove only Meta+Shift+S from Spectacle's **Launch Spectacle**
action, leaving its **Print Screen** binding in place.

Pegline registers the default shortcut when it is available and preserves
existing assignments. The installer does not take a shortcut from another app.
You can always capture from Pegline's tray menu.

## Use Pegline

1. Press **Meta+Shift+S** and drag over the area you want to capture. Release
   to save it, or press **Esc** to cancel.
2. Watch the preview fly into the line. Pegline stays open for ten seconds;
   moving the pointer away does not shorten that preview.
3. Click the card to copy the full image, then paste into an app that accepts
   images. You can also drag the card into an app or folder.

| Action | Result |
| --- | --- |
| Meta+Shift+S | Capture a region through Spectacle |
| Meta+Alt+T or left-click the tray icon | Show or hide the line |
| Hover at the top center of a screen | Reveal the line |
| Click a card | Copy the full image and its file URL |
| Double-click a card | Open in your default image viewer |
| Hold a card for 450 ms | Open Spectacle's annotation editor |
| Drag a card | Share its file; a file manager can copy or move it |
| Right-click a card | Copy, open, annotate, locate, save a copy, or take down |
| Click a card's cross | Take down that card and keep its file |

The tray menu also offers **Capture current screen**, **Open screenshot
folder**, and **Take all down (keep files)**. Saving a copy uses your configured
Pictures folder and a unique filename; existing files are never overwritten.

Captures made while Pegline is running open the line automatically. Images
already on disk restore quietly at startup. A new capture resets the ten-second
timer; an active card press, drag, or menu delays hiding until the interaction
can finish. A line revealed manually hides shortly after the pointer leaves.
Connected screens share the same recent cards.

Pegline watches Spectacle's configured image-save folder. It accepts PNG, JPEG,
WebP, BMP, GIF, and TIFF when installed Qt codecs can decode them, and ignores
videos, hidden files, symlinks, and incomplete writes. Files moved or deleted
elsewhere disappear from the line. Dismissal and capacity eviction keep the
originals. [Folder settings](docs/troubleshooting.md#screenshots-do-not-appear)
explain how to choose a different watch directory.

## Commands and startup

These commands control the running instance:

```sh
pegline --capture-region
pegline --show
pegline --hide
pegline --toggle
pegline --status
pegline --quit
```

`pegline --help` lists options. `--status` returns JSON for troubleshooting;
it includes local screenshot paths, so redact it before sharing.

The enabled user service starts Pegline with your **graphical login session**.
To manage it:

```sh
systemctl --user restart pegline.service
systemctl --user disable --now pegline.service
systemctl --user enable --now pegline.service
```

Use `bash linux/scripts/install.sh --no-start` to install files without
starting or enabling the service. It leaves an already running instance alone.
See [startup troubleshooting](docs/troubleshooting.md#pegline-does-not-start-at-login)
for diagnostics.

## Update or uninstall

From a clean checkout on `linux-pegline`, update and reinstall:

```sh
git pull --ff-only
bash linux/scripts/install.sh
```

If a checkout predates the privacy cleanup, follow the
[history-update instructions](docs/troubleshooting.md#git-update-fails-after-the-history-cleanup).

To uninstall, run this from the checkout:

```sh
bash linux/scripts/uninstall.sh
```

Uninstalling removes the executable, service, desktop entry, and icon. It keeps
screenshot files and saved history. Local history lives at
`${XDG_STATE_HOME:-$HOME/.local/state}/pegline/state.json`; it contains image
paths and file fingerprints, not image copies. Pegline uses no account,
telemetry, or network requests at runtime.

## Compatibility and attribution

KDE Wayland is the verified target, including capture and reveal on two
displays with mixed scaling. Other compositors, X11 behavior, and multiple
seats have not received equivalent live testing. Fullscreen stacking follows
the compositor's top-layer policy. The preview starts at screen center because
Spectacle exports do not reliably include the original selection rectangle.

The original macOS implementation remains in `Sources/Tendedero` and `scripts`
as reference. Build the Linux app from `linux/`. Pegline has its own name and
icon and is not endorsed by the upstream author. The upstream code is
[MIT licensed](LICENSE), with the original attribution preserved; the
Tendedero name, icon, and upstream documentation artwork have separate
restrictions. The vendored
[Wayland protocol](linux/ui/ext-data-control-v1.xml) retains its own license
and copyright notices.
