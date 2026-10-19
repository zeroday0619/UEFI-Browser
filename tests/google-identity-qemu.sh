#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
test_directory=$(mktemp -d)
processes=()
cleanup() {
  for process in "${processes[@]}"; do
    kill "$process" 2>/dev/null || true
    wait "$process" 2>/dev/null || true
  done
  rm -rf -- "$test_directory"
}
trap cleanup EXIT
clang -shared -fPIC -Wall -Wextra -Werror "$project/tests/identity-dns.c" -ldl -o "$test_directory/identity-dns.so"
python3 "$project/tests/identity-dns.py" > "$test_directory/dns.log" 2>&1 &
processes+=("$!")
python3 "$project/tests/https-fixture.py" --directory "$test_directory/tls" --identity-domains > "$test_directory/https.log" 2>&1 &
processes+=("$!")
for attempt in {1..100}; do
  for process in "${processes[@]}"; do
    if ! kill -0 "$process" 2>/dev/null; then
      cat "$test_directory/dns.log" "$test_directory/https.log"
      exit 1
    fi
  done
  if [[ -s "$test_directory/tls/ca.esl" ]] && rg -q 'HTTPS fixture ready' "$test_directory/https.log"; then break; fi
  sleep .1
done
rg -q 'HTTPS fixture ready' "$test_directory/https.log"
LD_PRELOAD="$test_directory/identity-dns.so" TEST_CA_FILE="$test_directory/tls/ca.esl" \
  QEMU_TEST_ACCEL="${QEMU_TEST_ACCEL:-kvm}" QEMU_TEST_CPUS="${QEMU_TEST_CPUS:-4}" \
  INPUT_SCENARIO=chrome-identity python3 "$project/tests/browser-qemu.py"
cat "$test_directory/dns.log" "$test_directory/https.log"
