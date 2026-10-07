#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cmake -S "$project_dir/ui" -B "$project_dir/target/ui-check" -G Ninja -DPEGLINE_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$project_dir/target/ui-check" --parallel 2
QT_QPA_PLATFORM=offscreen "$project_dir/target/ui-check/pegline-ui-smoke"
QT_QPA_PLATFORM=offscreen "$project_dir/target/ui-check/pegline-ui-smoke" --motion
QT_QPA_PLATFORM=offscreen "$project_dir/target/ui-check/pegline-ui-smoke" --arrival
