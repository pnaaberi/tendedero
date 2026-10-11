#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cmake -S "$project_dir/ui" -B "$project_dir/target/ui-check" -G Ninja -DPEGLINE_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$project_dir/target/ui-check" --parallel 2
QT_QPA_PLATFORM=offscreen "$project_dir/target/ui-check/pegline-ui-smoke"
QT_QPA_PLATFORM=offscreen "$project_dir/target/ui-check/pegline-ui-smoke" --motion
QT_QPA_PLATFORM=offscreen "$project_dir/target/ui-check/pegline-ui-smoke" --arrival
QT_QPA_PLATFORM=offscreen "$project_dir/target/ui-check/pegline-ui-smoke" --fileshare
QT_QPA_PLATFORM=offscreen "$project_dir/target/ui-check/pegline-ui-smoke" --videos
if command -v ffmpeg >/dev/null; then
  QT_QPA_PLATFORM=offscreen "$project_dir/target/ui-check/pegline-ui-smoke" --video-previews
else
  echo 'Skipping video frame previews: optional ffmpeg is unavailable'
fi
QT_QPA_PLATFORM=offscreen "$project_dir/target/ui-check/pegline-fileshare-smoke"
