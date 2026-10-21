# Inspector command and JIT transition costs

## Measured attribution

The optional `BLINK_TASK_DIAGNOSTICS=1` build now records each Inspector command
lasting at least 50 ms. The record separates debugger entry, command dispatch
and debugger exit. It logs the method name, call identifier and encoded message
size without recording the command payload. The V8 `Debugger.enable` path also
reports enable, compiled-script collection and script-reporting times.

Thread-local runtime counters measure calls to the protected JIT scope gate and
time spent in those calls. Disabled diagnostics bypass timestamp reads and
counter updates. Non-Blink runtime images provide a disabled flag. The counters
are diagnostic data in the renderer, not inputs to supervisor permission policy.
Wall time includes host scheduling, nested Inspector loops and other work on the
same logical thread. Timing overhead is present; the data is not an exclusive
CPU profile.

The `inspector-timing-youtube-suite.json` run completed at CPL3 in 198.85 seconds
without an unexpected renderer fault. `Debugger.enable` took 13.613 seconds.
The extended `inspector-cost-youtube-suite.json` run completed in 197.09 seconds
on image `a58e99918c591dfb459431b8d2e55089ffce59121580d18f238b4aa5ebad0f0f`.
It measured:

| Operation | Total | Internal attribution |
| --- | --- | --- |
| `Debugger.enable` | 13.424 s | V8 enable 13.275 s; collect 0.134 s; report 0.009 s; 561 scripts |
| JIT gate calls during `Debugger.enable` | 4,594 calls | 0.023 s |
| One `Runtime.evaluate` | 2.243 s | 327,044 JIT gate calls consumed 2.166 s |

These are distinct bottlenecks. The long V8 enable phase is not explained by
JIT gate time. The specific expensive callee within that phase remains to be
measured. The typed player-state query returned `YTSTATE 1 67.21902083333333 4
null`. This observation is not a sustained real-time playback or control test.

## Permission transition change

The supervisor previously reloaded CR3 after every JIT scope request and every
validated TLS switch. Nested entry/exit changes only a protected depth counter;
selecting a thread with the current JIT mode changes no leaf permissions.
`renderer_jit_select` now reports whether the mode changed. Those unchanged
paths skip the CR3 write. Actual mode changes still visit all marked JIT
branches, update alias permissions and invalidate translations before returning
to CPL3. Map, protection and broker paths retain their independent flushes.

All depth validation, identity-alias exclusion and TLS scope ownership remain
in supervisor memory. The optimization assumes the existing single renderer
processor per address space; it does not implement cross-processor TLB shootdown.

## Reproduction

Build the native archive, then create an isolated diagnostic image:

```sh
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/jit-transition" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/jit-transition" python3 validate-binary.py
```

With the fixture server running from `build/inspector-timing-https`:

```sh
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/jit-transition \
  --https-ca build/inspector-timing-https/ca.esl --prefix jit-transition \
  --require-cpl3 --require-no-renderer-faults devtools mse-batch
YOUTUBE_INSPECT=1 YOUTUBE_CONSOLE_TIMEOUT=120 \
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/jit-transition \
  --https-ca build/inspector-timing-https/ca.esl --prefix jit-transition-youtube \
  --require-cpl3 --require-no-renderer-faults youtube-observe
```

`inspector-cost-sources.json` and `jit-transition-sources.json` identify the
sources and images. The default `build/blink` image is unchanged.

## Verification of unchanged permissions

The host policy test passed with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
clang -O1 -g -fsanitize=address,undefined tests/jit-policy-host.c \
  -o build/jit-transition/host
build/jit-transition/host
```

The standalone CPL3 probe now includes three samples of 100,000 nested JIT
entry/exit pairs while an outer write scope remains open. It checks the page
permissions after the loop, then exercises thread-mode restoration and RX
execution. The existing intentional write/execute faults remain mandatory.
`tests/jit-qemu.py` accepts an isolated image directory and result prefix so
comparison runs retain independent artifacts.

Both images used the same embedded payload. A copied loader source tree restored
only the previous unconditional CR3 behavior for the baseline; production files
were not swapped during the comparison. `jit-transition-baseline-build.json`
records that source tree and the common payload hash. Each image completed all
14 CPL3 boundary cases, including rejection of executable-page writes,
execution of writable pages and writes through the identity alias.

| Nested pairs per sample | Baseline samples | Modified samples |
| --- | --- | --- |
| 100,000 | 44.604, 44.586, 44.655 ms | 26.824, 26.964, 27.006 ms |

The median fell from 44.604 to 26.964 ms, a 39.55% reduction for this isolated
path. This is one run per image with three samples, not a general workload speedup.
`jit-transition-comparison.json` preserves the raw results and image identities.

The full browser image
`cc4ae38c3d47e332c238556018652684e62c2e7b0f4965e48b0e90ccbcc562c5`
passed native/EFI build and static validation. `jit-transition-suite.json`
records DevTools in 89.56 seconds and queued-work MSE audio in 36.36 seconds,
both at CPL3 without unexpected renderer faults. DevTools exercised native
entry, Console DOM/CSS mutation, pause/step/resume, reopening and detachment.

## Live result after the transition change

`jit-transition-youtube-suite.json` records a 195.79-second run at CPL3 with no
unexpected renderer fault. The 90-second screenshot visibly contains the video
and YouTube page. The original Inspector frontend accepted a typed player query
and returned `YTSTATE 1 68.82779166666667 2 null`. Console readiness took 17.698
seconds. Renderer cleanup completed with acknowledgement, no forced NMI and
2.190 seconds of teardown.

The remaining costs were substantial: `Debugger.enable` took 12.473 seconds,
including 12.322 seconds inside the V8 enable phase. A `Runtime.evaluate` command
took 2.244 seconds with 307,966 JIT calls consuming 2.164 seconds. The latter is
not a meaningful improvement over the preceding live run. Live scripts and call
counts differ between runs, so these measurements do not form a controlled
browser-performance comparison. The isolated nested-scope improvement does not
resolve the cost of actual mode transitions or V8 initialization.

This scenario does not execute the full pause/resume/seek real-time gate.
Continuous real-time video/audio progression, synchronization and the other
implementation goals remain incomplete. The next performance investigation must
separate the work inside V8 enable and reduce actual JIT mode-transition costs
without weakening alias-wide W^X.

[Supervisor JIT indexing](JIT-INDEX.md) subsequently replaces repeated sparse
page-table walks during actual mode transitions with a protected index and
retains the original walk as a correctness fallback.
