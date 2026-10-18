#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd "$project/../.." && pwd)
scratch="$root/work/chromium-build"
"$project/prepare.py"
for component in skia v8; do
 (cd "$scratch/$component" && ../tools/gn gen out/uefi)
done
ninja -C "$scratch/skia/out/uefi" -j"${JOBS:-8}" skia
ninja -C "$scratch/v8/out/uefi" -j"${JOBS:-8}" v8_monolith
