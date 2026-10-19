#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
test_directory=$(mktemp -d)
trap 'rm -rf -- "$test_directory"' EXIT
clang -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$project/src/network" "$project/tests/eevdf.c" -o "$test_directory/eevdf"
"$test_directory/eevdf"
# The assembly coroutine switches stacks without ASan fiber instrumentation.
clang -O2 -Wall -Wextra -Werror -fsanitize=undefined \
  -I "$project/runtime" -I "$project/src/network" \
  "$project/tests/network-async.c" "$project/src/network/async.c" -o "$test_directory/async"
"$test_directory/async"
python3 "$project/tests/process-simd.py"
