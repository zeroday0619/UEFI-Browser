#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
test_directory=$(mktemp -d)
trap 'rm -rf -- "$test_directory"' EXIT
"${CXX:-clang++}" -std=c++20 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -I "$project/src" \
  "$project/tests/pump-loop.cc" -o "$test_directory/pump-loop"
"$test_directory/pump-loop"
