#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
input_arguments=()
case "${ENABLE_MOZC:-1}" in
  1) input_arguments+=(--mozc) ;;
  0) ;;
  *) printf '%s\n' 'ENABLE_MOZC must be 0 or 1.' >&2; exit 1 ;;
esac
python3 "$project/build-blink-libraries.py" --resolve-inputs --jobs "${JOBS:-8}" "${input_arguments[@]}"
ENABLE_BLINK=1 BUILD_DIRECTORY="$project/build/blink" "$project/build-musl.sh"
BUILD_DIRECTORY="$project/build/blink" python3 "$project/validate-binary.py"
