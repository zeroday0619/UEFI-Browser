# Renderer memory pressure and page-table growth

Live YouTube runs exposed failures in both Skia scroll-scene serialization and V8 garbage-collection worklist allocation. A diagnostic allocator recorded a rejected 65,568-byte physical backing request while the C heap held 715.82 MiB against its 840.28 MiB requested-byte limit. This ruled out that C heap limit for the recorded failure; firmware and supervisor allocation failures use the same EFI out-of-resources status.

## Empty physical batches

The renderer used 1 MiB physical batches for virtual page commitments. It reused released pages but retained every batch until process teardown. `src/renderer_pages.h` now returns an entirely empty batch to the broker immediately. Partially occupied batches remain valid. Existing unmap and discard paths revoke virtual aliases before returning physical backing.

`tests/renderer-pages-host.py` tests the production pool under AddressSanitizer and UndefinedBehaviorSanitizer with fault-injected firmware. It checks zeroing, partial-batch lifetime, eight allocation/release cycles and failed expansion. The previous implementation fails the same test's empty-middle-batch assertion; see `renderer-page-reclaim-baseline.log` and `renderer-page-reclaim-host.log`.

The `page-reclaim-probe-suite.json` QEMU run passed 14 sandbox cases and asynchronous scrolling. Its memory probe repeatedly committed and released 64 MiB, returning to 32 MiB of physical batches retained by other live mappings and page tables. The seven normal regressions in `page-reclaim-normal-suite.json` passed, including actual Opus AC'97 output. The live YouTube run still failed, so empty-batch reclamation alone did not resolve the site failure.

## Independent page-table exhaustion

The loader previously preallocated 2,048 supervisor page-table pages and never extended that pool. A later live run recorded an abort caller inside `invalidate()`, after a rejected supervisor mapping operation, while the broker held 2,034,790,400 bytes, below its 2 GiB data quota.

The isolated `build/page-table-baseline-final` memory probe reserves 4.5 GiB of virtual address space and commits one 4 KiB page every 2 MiB. The fixed-pool implementation fails before completing all 2,304 mappings. Its serial log records `VM: map rejected status=-12` while broker backing totals only 34,041,856 bytes. This reproduces page-table exhaustion independently of large site data allocations. Earlier diagnostic stdout was buffered and omitted the mapping reason at abort; the failure diagnostic now uses stderr, and `abort()` records its direct caller before attempting frame walking.

## Protected growth

The loader now allocates page-table backing in 1,024-page chunks on the BSP, with a separate 16,384-page (64 MiB) ceiling. Identity grants reserve enough table pages before changing their mappings. An AP mapping operation with fewer than three table pages available sends a fixed, value-only reserve request through the protected broker slot and waits for the BSP. Firmware allocation never executes on the AP.

Only the loader owns table pointers and allocation metadata. Table backing remains supervisor-only and is released during quiescent renderer teardown. The renderer-visible bootstrap does not expose the reserve callback. The broker accepts only zero-valued reserve arguments and replenishes a fixed minimum; it does not accept addresses, caller-selected sizes or function pointers. Existing grant validation and JIT alias permissions remain in place. This change does not complete the sandbox audit.

## Reproduction

```sh
python3 tests/renderer-pages-host.py
python3 tests/allocator-host.py
ENABLE_BLINK=1 ENABLE_MEMORY_PROBE=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/page-tables-probe" ./build-musl.sh
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/page-tables" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/page-tables" python3 validate-binary.py
```

Run the HTTPS fixture server before the state suites:

```sh
python3 tests/network-servers.py --directory build/page-tables-https
```

```sh
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --https-ca build/page-tables-https/ca.esl \
  --build-directory build/page-tables-probe --prefix page-tables-probe \
  --require-cpl3-probe --require-memory-probe sandbox renderer-scroll
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --https-ca build/page-tables-https/ca.esl \
  --build-directory build/page-tables --prefix page-tables-normal \
  --require-cpl3 --require-no-renderer-faults \
  devtools-async devtools renderer-scroll workers webgl webgpu mse-audio
```

Use a fresh suite prefix when rerunning raw-disk cases. The new memory probe checks zeroed commitment and reclamation, valid and rejected reserve requests, and all 2,304 sparse mapping contents before unmapping them. Suite acceptance requires its completion marker.

The normal EFI image is `4db0b7235b57117ca4202dc9114e9a9d5628a47d1cf161ead6f24261108eba32`, with payload `e5b4c27f006b4ffbf4d464571e1451ff52e4c1fd86c9a436129895dbb87ef4d6`. The probe EFI is `a5c4261edbff5acf9ee9461199b5d590535f17f4cfbbda1127a2dfb945f94445`. These are separate test images; the default image has not been replaced.

## Results and remaining pressure

The growth probe completed all 2,304 mappings and repeated 64 MiB reclamation checks. `page-tables-check-probe-suite.json` records a passing sandbox run with all 14 startup boundary cases, supervisor-read and protected-page-table-write denial, and two interactive recoveries. `page-tables-probe-suite.json` also records a passing asynchronous-scroll case. Its first sandbox run failed because the test checked for a click acknowledgment immediately after sending mouse-up. The log showed button focus, but the asynchronous event had not yet completed. All six recovery-click checks now wait for a new acknowledgment within five seconds. The rerun passed without changing the guest image.

The live growth image still exhausts its separate data quota before the 180-second observation. `page-tables-youtube.log` records a 1,642,496-byte request rejected with 2,147,373,056 bytes charged against the 2,147,483,648-byte broker limit, followed by a renderer abort and recovery. This is now an explicit quota failure, unlike the earlier fixed-table-pool rejection. The 120-second screenshot shows the live video surface and page UI, but neither this frame nor MSE initialization proves sustained playback. Pre-exhaustion cache eviction, managed-heap pressure notification and continued playback validation remain required.

`page-tables-check-normal-suite.json` passes all seven normal cases with CPL3 execution and no unexpected renderer faults: Inspector stack capture, DevTools interaction, asynchronous scrolling, workers, WebGL, WebGPU and MSE audio. The audio check verifies actual captured Opus AC'97 stereo output, continuity, mute and attenuation. During the six-second JavaScript scroll fixture, the observed first screen change was 24.11 ms, with no reconciliation channel differences greater than two and unchanged fixed controls. This is QMP observation, not physical display latency. Native compilation, EFI linking and the static binary validator passed. `page-tables-sources.json` records source and image hashes and matching native runtime headers.
