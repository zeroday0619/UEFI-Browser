# Supervisor JIT page index

## Reason for the change

The live Inspector measurements in `INSPECTOR-COST.md` attributed 2.164 seconds
of a 2.244-second Console evaluation to 307,966 JIT scope calls. Skipping
redundant CR3 reloads improved nested requests but did not remove the cost of
actual mode transitions. The prior transition path walked each marked
page-table branch and looked up every alias's physical identity through the
page-table hierarchy.

## Protected index and fallback

`src/loader-jit.h` maintains a supervisor-owned dense index of arena PTE
addresses and their physical identity PTEs. The hash table deduplicates arena
slots. Executable registration indexes preexisting aliases; a new JIT mapping
indexes its slot after mapping. Remapping the same slot refreshes its identity
pointer. Renderer-controlled pointers never populate this index.

Each transition reads the current leaf metadata. Cleared or ordinary reused
slots are skipped. JIT leaves retain the existing desired-permission bits;
read-only aliases remain read-only, and identity aliases remain NX. Actual mode
changes still reload CR3 before returning to CPL3. A missing identity,
inconsistent physical identity or full index selects the existing marked-tree
walk for the remainder of the address-space lifetime. Partial indexed updates
are followed by the complete walk before user execution resumes.

The index has 16,384 records and 32,768 hash slots, using 384 KiB of fixed loader
storage on x86-64. This bounds privileged memory use. Historical slots remain
until address-space reset; exhausting them reduces performance but does not
omit permission updates. Registration still walks existing aliases to discover
newly executable backing. This change does not optimize that discovery path.

Protected page-table pages are allocated monotonically and remain allocated
until renderer teardown. The index is reset before a new sandbox receives its
arena or grants. Independent table reclamation would require invalidating
these pointers first. The current execution model still has one processor
executing the renderer address space; parallel execution would require a TLB
shootdown design and synchronization around permission updates.

## Host and QEMU boundary checks

The production-policy host tests passed with AddressSanitizer and
UndefinedBehaviorSanitizer:

```sh
clang -O1 -g -fsanitize=address,undefined tests/jit-policy-host.c \
  -o build/jit-index/policy-host
build/jit-index/policy-host
clang -O1 -g -fsanitize=address,undefined tests/jit-index-host.c \
  -o build/jit-index/index-host
build/jit-index/index-host
```

The index-specific test covers preexisting aliases, new code, remapping to
different backing, revocation, ordinary reuse, inconsistent-cache fallback,
capacity fallback and reset before table release. Repeated indexed transitions
also assert that no physical-identity lookup occurs. The existing policy test
retains nested scopes, TLS-mode restoration, depth overflow and slot exhaustion.
`tests/jit-index-differential.c` additionally performs 10,000 deterministic
mapping, unmapping, revocation and mode-transition operations. It includes
non-present leaves and checks every indexed leaf/identity result against the
original marked-tree transition policy. This also passed ASan and UBSan:

```sh
clang -O1 -g -fsanitize=address,undefined tests/jit-index-differential.c \
  -o build/jit-index/differential-host
build/jit-index/differential-host
```

The CPL3 probe maps 64 additional executable aliases into separate leaf tables,
for 66 total aliases of one backing page. In each of three samples it performs
1,000 write/execute transition pairs. Every pair writes an instruction operand
through the writable alias, closes the scope, executes the RX code and checks
its return value. The probe checks final alias permissions and unmaps them
before the existing discard/reuse checks.

The baseline, indexed and forced-fallback loaders share one embedded payload.
The baseline uses a copy of the pre-index loader sources; the forced-fallback
loader is compiled with `RENDERER_JIT_INDEX_CAPACITY=8`. All three runs passed
14 CPL3 boundary cases, including blocked executable-page writes, blocked
execution inside write scopes and blocked identity-alias writes.

| Loader | Transition sample times | Median |
| --- | --- | --- |
| Previous marked-tree walk | 14.608, 14.486, 14.508 ms | 14.508 ms |
| Protected index | 0.833, 0.830, 0.830 ms | 0.830 ms |
| Eight-record fallback | 14.715, 14.689, 14.760 ms | 14.715 ms |

The indexed path reduced this isolated median by 94.28%. These are three samples
from one run per loader, not a browser-wide performance claim. Raw measurements
are in `jit-index-comparison.json`; exact loader commands and the shared payload
hash are in `jit-index-build-comparison.json`. Reversing
`jit-index-change.patch` against the tested sources reconstructs the measured
baseline loader policy.

The updated verifier can require the transition samples explicitly:

```sh
ENABLE_RENDERER_CPL3=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/jit-index-boundary" ./build-musl.sh
python3 tests/jit-qemu.py --build-directory build/jit-index-boundary \
  --prefix jit-index-checked --require-transition-benchmark
```

These checks cover the stated permission and lifecycle paths. They are not a
complete sandbox audit, cross-processor race proof or independent security
review. The full implementation goals remain active.

## Browser build and local regression

The isolated browser image has SHA-256
`3eb716da8f58f150a6b528292321904963b0433245d472243f52dfadb5f9c540`.
EFI linking and static binary validation passed. `jit-index-suite.json` records
DevTools in 87.18 seconds and queued-work MSE audio in 36.36 seconds, both at
CPL3 without unexpected renderer faults. The updated transition-sample verifier
also passed for the indexed boundary image in `jit-index-checked.json`.

```sh
BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/jit-index" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/jit-index" python3 validate-binary.py
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/jit-index \
  --https-ca build/inspector-timing-https/ca.esl --prefix jit-index \
  --require-cpl3 --require-no-renderer-faults devtools mse-batch
```

The local fixture server uses `python3 tests/network-servers.py --directory
build/inspector-timing-https`. `jit-index-sources.json` records source and image
hashes. The default `build/blink` image was not replaced.

## Initial live-site observation

`jit-index-youtube-suite.json` records a 197.78-second run with renderer CPL3
execution and no unexpected faults. The player-state query returned
`YTSTATE 3 72.6586875 2 null`; at that instant the player was buffering with no
media error. Renderer cleanup completed without forced termination in 1.769
seconds. A long Console evaluation took 0.459 seconds, including 155,680 JIT
scope calls taking 0.407 seconds. V8 enable still took 13.228 seconds, including
149,912 JIT calls taking 0.324 seconds.

Those live call counts differ from the previous run, so the evaluation times
cannot be treated as a controlled speedup. The 94.28% result above applies only
to the sparse-alias microbenchmark. The live observation does not prove
sustained real-time playback.

## Full live playback gate

A fresh-profile run enabled `YOUTUBE_LONG=1`, `YOUTUBE_INSPECT=1`,
`YOUTUBE_PLAYBACK_PROBE=1`, `YOUTUBE_CONSOLE_TIMEOUT=120` and
`QEMU_TEST_REQUIRE_WATCHDOG_OFF=1`, then ran `tests/state-suite.py` with
`--cpus 4 --workers 1 --build-directory build/jit-index`, the same fixture CA,
`--prefix jit-index-playback --require-cpl3 --require-no-renderer-faults
youtube-observe`.

`jit-index-playback-suite.json` records **failure** after 498.46 seconds, with
CPL3 execution and no unexpected renderer fault. Eleven typed Console samples
completed. Video frames decoded, pause held media time, resume advanced, and
seeking to 120 seconds completed. The real-time checks failed:

| Interval | Page-clock seconds | Media seconds | Ratio |
| --- | --- | --- | --- |
| Playing | 21.1001 | 10.2599 | 0.48625 |
| Resumed | 21.1784 | 11.7200 | 0.55339 |
| After seek | 16.9007 | 9.00925 | 0.53307 |

The playing and resumed gates require a ratio from 0.9 through 1.1. All samples
reported no media error. A/V synchronization and direct pointer control of the
live player were not verified. The 180-second screenshot was visually inspected
and contains the movie frame and YouTube page, with missing sidebar images.
Console readiness took 32.900 seconds; V8 enable consumed 23.189 seconds while
its measured JIT calls consumed only 102 microseconds.

The test switched to the video tab during waits and to Inspector for commands.
This diagnostic-enabled run is not a controlled comparison against earlier
non-diagnostic images or changing live-site content. Diagnostic overhead and
those workload differences must be separated before attributing playback-rate
changes to the index. The implementation does not establish real-time playback.

`jit-index-playback-task-costs.json` aggregates only reported slow tasks, not
exclusive CPU time or total workload share. There were 1,305 slow frame-record
reports totaling 98.172 seconds of wall time, including 32.725 seconds in page
painting and 58.422 seconds in scroll-scene capture/reflow. The task observer
labels their posting location as `FrameLoop::Layout`; the executed callback is
the subsequent recording phase. These measurements identify remaining work
without proving that a single path accounts for every media stall.

## Other engine regressions

`jit-index-engines-suite.json` records workers (21.38 s), WebGL (21.06 s) and
WebGPU (21.37 s), all passing at CPL3 without unexpected faults. GPU tests use
software rendering; they provide no hardware-acceleration evidence.

The first live sandbox-recovery invocation incorrectly used the ordinary image
without `--require-cpl3-probe`. It stopped at the harness precondition in 5.34
seconds before fault injection. `jit-index-recovery-suite.json` retains that
failure. This is not a renderer fault or a passing recovery check.

The corrected build and invocation were:

```sh
BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/jit-index-recovery" ./build-musl.sh
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/jit-index-recovery \
  --https-ca build/inspector-timing-https/ca.esl --prefix jit-index-recovery-probe \
  --require-cpl3-probe sandbox
```

`jit-index-recovery-probe-suite.json` passed in 34.27 seconds on image
`3c11bd958b31524497955305f6f231ae6ae4e03fdf4b9b66e6b69910e3444c74`.
All 14 startup boundary cases passed. Live supervisor-memory reads and
page-table writes faulted with the expected page-fault codes 5 and 7 at CPL3.
Each deliberate fault was followed by renderer recreation, page reload and a
successful button click. All three observed renderer instances ran at CPL3.
These intentional faults are expected by this scenario.

The default EFI hash remains
`94d711c0fe3942329da0313acd8512a46444cc1b60d5373c79111c64f021d978`.
No source changes were staged or committed, and no dependencies were added.
