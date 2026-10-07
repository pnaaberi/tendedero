#!/usr/bin/env bash
set -euo pipefail
systemctl --user disable --now pegline.service
rm -f -- "${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/pegline.service" \
  "$HOME/.local/bin/pegline" \
  "${XDG_DATA_HOME:-$HOME/.local/share}/applications/org.choppy.Pegline.desktop" \
  "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/scalable/apps/pegline.svg"
systemctl --user daemon-reload
printf 'Removed Pegline. Screenshot files and saved history were kept.\n'
