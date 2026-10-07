# Troubleshooting Pegline

[Back to the README](../README.md)

Run these commands from a terminal inside your KDE login session. Pegline
needs the graphical session's D-Bus and Wayland connection. Use the same normal
user that installed it.

## Quick diagnosis

```sh
systemctl --user status pegline.service --no-pager
~/.local/bin/pegline --status
journalctl --user -u pegline.service -n 50 --no-pager
```

When the app is running, `--status` reports:

| Field | Meaning |
| --- | --- |
| `platform` | `wayland` for the supported desktop session |
| `watch` | The screenshot directory actually being monitored |
| `items` | Retained cards with file paths, `modified` timestamps, `size` in bytes, and `new` capture flags |
| `visible` | Whether the line is revealed on any screen |
| `capturing` | Whether a Pegline-launched Spectacle capture is in progress |
| `surfaces` | Per-screen size, reveal state, and `flying` animation state |
| `error`, `warning` | Watcher errors or preserved-state recovery notices |

`--status` exits with code 3 if Pegline is not running. A hidden line with
`visible: false` is normal: use the tray icon, Meta+Alt+T, or hover or click the
small green marker at a screen's top center to reveal it. Only the visible
marker responds; the surrounding transparent edge passes clicks through.

Status, history, and journal output can contain personal image paths. Redact
them before opening a public issue; do not paste raw state files or desktop
captures into GitHub.

## Meta+Shift+S opens Spectacle or does nothing

1. Confirm `pegline.service` is active. Find Pegline in the tray's expanded
   entries if its icon is hidden.
2. Open **System Settings → Keyboard → Shortcuts**. Under **Spectacle**, remove
   **Meta+Shift+S** from **Launch Spectacle**, retaining **Print Screen**.
3. Under **Pegline**, assign **Meta+Shift+S** to **Capture a region to Pegline**
   and **Meta+Alt+T** to **Show or hide Pegline**. Apply the changes.
4. Test the capture shortcut again, or isolate the shortcut from capture with:

   ```sh
   ~/.local/bin/pegline --capture-region
   ```

If the command works, the remaining problem is the KDE binding. Pegline does
not replace another application's shortcut automatically. If capture itself
fails, confirm `spectacle --help` works and check the service journal.

Release the mouse button to complete a region selection. **Esc** cancels it.
While a capture is open, further Pegline capture requests are ignored until it
finishes or is cancelled.

## Screenshots do not appear

Check the `watch` field in `pegline --status` and compare it with the image-save
location in Spectacle's settings. The tray's **Open screenshot folder** opens
the monitored directory.

Pegline reads Spectacle's `imageSaveLocation` from the `[ImageSave]` section of
`${XDG_CONFIG_HOME:-$HOME/.config}/spectaclerc` when it starts. The fallback is
`$HOME/Pictures/Screenshots`. Restart Pegline after changing Spectacle's
location:

```sh
systemctl --user restart pegline.service
```

The directory must be readable, and images must be complete, nonempty regular
files. Hidden files, symlinks, videos, and unsupported formats are ignored.
Pegline samples every 500 ms and waits for two matching file samples, so allow
a short delay after a screenshot is saved. Stable images that repeatedly fail
to decode produce an error and remain eligible for retry.

If a file is present but the line stays hidden at startup, that is expected:
existing images restore quietly. Captures created after startup trigger the
flight and ten-second reveal.

### Try a different watch folder

Stop the service before launching another instance with `--watch`:

```sh
systemctl --user stop pegline.service
~/.local/bin/pegline --watch "$HOME/Pictures/Screenshots" --daemon
```

The foreground process runs until you press Ctrl+C. Restore the installed
service afterward with `systemctl --user start pegline.service`.

`--watch` changes monitoring only. It does not change where Spectacle saves
captures. Match the watch directory to Spectacle's save location if you want
Pegline's capture actions to feed the line. Passing `--watch` to an already
running instance does not change its directory.

### Keep a watch-folder override across logins

Create a service override:

```sh
systemctl --user edit pegline.service
```

Use this example, replacing the directory as needed:

```ini
[Service]
ExecStart=
ExecStart=%h/.local/bin/pegline --daemon --watch "%h/Pictures/Screenshots"
```

The empty `ExecStart=` clears the original command. `%h` expands to your home
directory; systemd does not run this command through a shell. Apply the change:

```sh
systemctl --user daemon-reload
systemctl --user restart pegline.service
```

For [systemd service-command syntax](https://man.archlinux.org/man/systemd.service.5.en#COMMAND_LINES),
use specifiers such as `%h` rather than shell substitutions. Service overrides
survive reinstalling Pegline.

## Pegline does not start at login

The installer enables a systemd **user** service tied to the graphical
session. Check it inside KDE:

```sh
systemctl --user is-enabled pegline.service
systemctl --user status pegline.service --no-pager
systemctl --user status graphical-session.target --no-pager
```

Enable and start it if necessary:

```sh
systemctl --user enable --now pegline.service
```

If the service is missing, rerun `bash linux/scripts/install.sh` from the
checkout. If it failed, inspect the journal from [Quick diagnosis](#quick-diagnosis).
Starting the app at a text console does not provide its required graphical
session connections.

Quitting Pegline or stopping its service ends the current run; the enabled
service can start again at your next graphical login. Disable autostart with
`systemctl --user disable --now pegline.service`.

## Pegline command not found

Check the installed executable directly:

```sh
~/.local/bin/pegline --help
```

If that works, add this line to the startup file for your shell, such as
`~/.bash_profile` for Bash login shells or `~/.zprofile` for Zsh:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

Sign out and back in so KDE and newly opened terminals inherit the updated
PATH. Running that line in a terminal updates only that terminal and its
children. The user service uses an explicit executable path.

## Copy, annotation, or locating a file fails

Clicking copies a full PNG plus a file URL. The receiving application must
support image paste or file URLs; try another image-capable application to
separate clipboard support from a receiving-app limitation.

On Wayland, background clipboard access requires the compositor's
`ext-data-control-v1` protocol. Other compositors or older desktops may not
provide it. A native clipboard failure is reported rather than displaying
success. The [development guide](development.md#desktop-checks) includes a
Wayland failure check.

Annotation requires Spectacle's existing-image editor. **Show in folder**
launches Dolphin with the file selected; install Dolphin if your KDE setup
omits it. Both actions need the source image to remain readable. Saving a copy
also requires your Pictures directory to be writable.

## The line hides later than ten seconds

Each new capture resets the timer. A held card, drag, or open card menu postpones
hiding while the interaction is active. Dismiss the menu or finish the drag to
allow hiding. Meta+Alt+T hides the line early without deleting files.

The timer applies to new captures. A manually revealed line uses the shorter
pointer-leave delay.

## Saved history reports corruption

Pegline preserves an invalid history file beside
`${XDG_STATE_HOME:-$HOME/.local/state}/pegline/state.json` as
`state.corrupt-<timestamp>.json` and rebuilds its list from readable images.
The warning identifies that local backup. The original screenshot files are
kept. Corrupt backups can contain private paths, so keep them local.

## Git update fails after the history cleanup

The `linux-pegline` history was rewritten to remove machine references from
earlier documentation. A checkout made before that cleanup can diverge from
the current remote, causing `git pull --ff-only` to refuse the update.

Keep the old checkout if it contains local work, and clone a fresh copy into
an unused directory:

```sh
git clone --branch linux-pegline https://github.com/pnaaberi/tendedero.git pegline-current
cd pegline-current
bash linux/scripts/install.sh
```

Reinstalling updates the executable while preserving screenshots and saved
history. Avoid forcing a pull over changes you want to keep.

## Report a problem

Open an [issue](https://github.com/pnaaberi/tendedero/issues) with the steps to
reproduce it, the expected result, the actual result, and your Pegline,
Plasma, Qt, and Spectacle versions. Include only the relevant, redacted error.
Use a synthetic image if a screenshot is needed. Exclude usernames, home
paths, hostnames, device identifiers, tokens, and unrelated desktop content.
