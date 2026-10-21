# Ordered virtual reservations

## Evidence and implementation

After the grant-revocation optimization, the corrected-coordinate YouTube Inspector check still timed out waiting for a Console prompt. `grant-revoke-console-failure-backtrace.log` catches the renderer in `virtual_memory_map`, called by V8 semispace page allocation. Disassembly places the sampled instruction in the reservation overlap scan.

Previously the gap search restarted a scan of all 4,096 slots every time its candidate overlapped another reservation. Adjacent occupied intervals therefore required repeated full scans. `src/virtual_regions.h` now keeps an ordered index of stable region slots. Containment uses binary search, and gap selection advances through ordered intervals once. Insertions and removal update the index; a partial prefix or suffix trim preserves order, while a middle unmap inserts the right-hand interval. Teardown removes the corresponding index entries.

The initial ordered-index image retained the arena size, 4,096-slot limit, address-hint behavior, supported flags and page permissions. It did not add support for unmapping across multiple reservations or expand existing MAP_FIXED semantics. The later [dynamic registry](DYNAMIC-REGIONS.md) removes the fixed slot limit while preserving supervisor mapping validation.

## Host verification

`python3 tests/virtual-regions-host.py` compiles the production header with ASan and UBSan. It compares 30,000 deterministic randomized operations against an independent page-ownership bitmap and checks partial unmap, splits, containment, gaps, hints, reverse insertion, full capacity, slot reuse and address overflow. The check passed without sanitizer findings.

The same host executable performs 32 gap searches over 4,096 adjacent intervals. The original algorithm took 299,801 microseconds; the ordered algorithm took 274 microseconds. These measurements use a sanitizer build and support only this search workload, not general browser speed or QEMU playback performance. The complete output is in `virtual-regions-host.log`.

## EFI verification boundary

The normal image is `build/ordered-regions`, EFI SHA-256 `378de4f8fa7b5eae05260787f94af8dd87e963ee2751fcfeb3c4de3a4b2ae2ec`. Source and image hashes are recorded in `ordered-regions-sources.json`. EFI linking and static binary validation passed; no Chromium native-library source changed.

The initial QEMU probe in `ordered-regions-probe-suite.json` passed 14 CPL3 boundary cases, fault recovery, physical reclamation and sparse page-table growth. Seven probe invocations created and released 1,024 reservations and reused a hinted hole in 815–853 microseconds each. These timings were collected before concurrent live-site testing.

The extended probe also checks MAP_FIXED replacement zeroes only the replaced page, a middle unmap splits the reservation, protection fails for the hole, a hinted mapping reuses it and both neighboring page contents survive. The final probe and browser/live-site results are recorded separately below.

`ordered-regions-final-probe-suite.json` passes the extended split/replacement assertions and all 14 sandbox boundary cases with live fault recovery. This final probe includes both reservation tests. Its seven reservation timing samples are recorded as diagnostics; concurrent browser runs make them unsuitable for a controlled performance comparison.

## Reproduction

These commands describe the ordered-index revision. Current sources include the
dynamic registry and a 5,000-reservation probe; use `DYNAMIC-REGIONS.md` to reproduce
that revision rather than overwriting the historical image directories below.

```sh
python3 tests/virtual-regions-host.py
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/ordered-regions" ./build-musl.sh
ENABLE_BLINK=1 ENABLE_MEMORY_PROBE=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/ordered-regions-final-probe" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/ordered-regions" python3 validate-binary.py
```

Start `python3 tests/network-servers.py --directory build/ordered-regions-https` in another terminal, then use fresh prefixes for another QEMU run:

```sh
python3 tests/state-suite.py --workers 1 --cpus 4 \
  --https-ca build/ordered-regions-https/ca.esl \
  --build-directory build/ordered-regions-final-probe --prefix ordered-regions-probe-repeat \
  --require-cpl3-probe --require-memory-probe sandbox
python3 tests/state-suite.py --workers 1 --cpus 4 \
  --https-ca build/ordered-regions-https/ca.esl \
  --build-directory build/ordered-regions --prefix ordered-regions-normal-repeat \
  --require-cpl3 --require-no-renderer-faults \
  devtools renderer-scroll workers webgl webgpu mse-audio
```
