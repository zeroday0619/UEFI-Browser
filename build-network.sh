#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd "$project/../.." && pwd)
if [[ ! -f "$root/work/chromium-build/v8/out/musl/obj/libv8_monolith.a" || ! -f "$root/work/chromium-build/skia/out/musl/libskia.a" ]]; then
 "$project/build-v8-musl.sh"
fi
mkdir -p "$project/build"
python3 "$project/tools/ca_bundle.py" "${UEFI_CA_BUNDLE:-$project/trust/mozilla-ca.pem}" "$project/build/ca.esl"
ENABLE_NETWORK=1 ENABLE_V8=1 ENABLE_SKIA=1 BUILD_DIRECTORY="$project/build/network" "$project/build-musl.sh"
BUILD_DIRECTORY="$project/build/network" python3 "$project/validate-binary.py"
