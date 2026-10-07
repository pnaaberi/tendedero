#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
start=true
if [[ ${1-} == --no-start ]]; then
  start=false
elif [[ $# -ne 0 ]]; then
  echo 'Usage: install.sh [--no-start]' >&2
  exit 2
fi
cargo build --manifest-path "$project_dir/Cargo.toml" --release --locked
bin_dir="$HOME/.local/bin"
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}"
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}"
mkdir -p "$bin_dir"
install -m755 "$project_dir/target/release/pegline" "$bin_dir/pegline.new"
mv -f -- "$bin_dir/pegline.new" "$bin_dir/pegline"
install -Dm644 "$project_dir/data/pegline.svg" "$data_dir/icons/hicolor/scalable/apps/pegline.svg"
install -Dm644 "$project_dir/data/org.choppy.Pegline.desktop" "$data_dir/applications/org.choppy.Pegline.desktop"
install -Dm644 "$project_dir/data/pegline.service" "$config_dir/systemd/user/pegline.service"
if command -v kbuildsycoca6 >/dev/null; then kbuildsycoca6 --noincremental >/dev/null 2>&1; fi
systemctl --user daemon-reload
if $start; then
  "$bin_dir/pegline" --quit
  for attempt in {1..20}; do
    if ! busctl --user status org.choppy.Pegline >/dev/null 2>&1; then break; fi
    sleep 0.1
  done
  systemctl --user enable pegline.service
  systemctl --user restart pegline.service
fi
printf 'Installed Pegline: %s\n' "$bin_dir/pegline"
