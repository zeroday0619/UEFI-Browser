#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
python3 "$project/build-blink-libraries.py" --resolve-inputs --jobs "${JOBS:-8}"
ENABLE_BLINK=1 BUILD_DIRECTORY="$project/build/blink" "$project/build-musl.sh"
BUILD_DIRECTORY="$project/build/blink" python3 "$project/validate-binary.py"
