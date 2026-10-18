#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd "$project/../.." && pwd)
v8="$root/work/chromium-build/v8"
clang++ -fuse-ld=lld -std=c++20 -O2 -fno-rtti -I"$v8/include" -I"$v8/out/uefi/gen/include" \
 "$project/tests/v8-host.cpp" "$project/src/v8_probe.cpp" "$v8/out/uefi/obj/libv8_monolith.a" \
 -pthread -ldl -latomic -o "$project/build/v8-host"
"$project/build/v8-host"
