# Inspector input diagnosis on YouTube

The local DevTools UI regression passes on the default image. Earlier live YouTube runs failed to display an editable Console prompt within 30 seconds. The later [broker allocation index](BROKER-ALLOCATION-INDEX.md) image completes Console keyboard input and returns a player-state result, but input delays of approximately 487 ms remain. The historical failures below are retained as diagnosis evidence; a loaded frontend alone is not evidence that its panels work.

## Reproduction evidence

The diagnostic EFI image in `build/pointer-trace` has SHA-256 `61d5129bb70543c3e604b4d2fb0b6255ebd4db017107807cde8f4ea360d6bcff`. Its matching payload has SHA-256 `7c8fa115a3c57e2a15939869b2444503be21cf6f1faef194f8c97c6b6149c3f3`. It adds button-transition logging after firmware TPL restoration and at Blink input dispatch. Those temporary production diagnostics were removed after collecting the evidence; the diagnostic binaries and logs remain available.

`pointer-trace.log` records correct native button coordinates. In that run the test stopped before selecting Console because it waited only five seconds after `DEVTOOLS: frontend loaded`; `Main._showAppUI` took nine seconds. The observer now waits for the rendered Elements-tab underline with a bounded deadline and accounts for the optional language banner.

`pointer-trace-second.log` records the Console click at firmware coordinates `(143, 96)` and Blink coordinates `(143, 16)`. The 80-pixel difference is the native toolbar. Both button transitions reached Blink. The [Console-ready screenshot](pointer-trace-second-input-youtube-console-ready.png) shows the cursor at that tab, while Elements remains selected and its body is blank. This run contradicts the hypothesis that incorrect pointer coordinates alone cause the failure.

`pointer-trace-stack-backtrace.log` samples the renderer waiting at the privileged broker boundary. A separate 90-second reproduction, `pointer-trace-request-backtrace.log`, instead samples CPL3 execution in `V8Debugger::asyncTaskScheduledForStack`, `Isolate::CaptureDetailedStackTrace`, `ThreadIsolation::StartOfJitAllocationAt` and `firmware_jit_write_scope`. The second sample's broker slot is not pending. These samples do not establish a permanent broker deadlock.

## Metadata scope correction

The UEFI V8 write scope changes all registered executable aliases between RX and RW/NX. V8 also used that scope for CFI metadata access, including stack-frame lookup. This port allocates that metadata in ordinary writable data memory; changing executable aliases does not protect it.

`patches/blink-v8-metadata.patch` separates the metadata scope from executable-code permissions. It also rejects a non-null protected metadata allocator at initialization, because such an allocator would require a separate permission implementation. Actual code writes retain `RwxMemoryWriteScope` and the supervisor's alias-wide W^X policy. Host snapshot generators retain their original behavior. This change does not implement protected CFI metadata or complete the security sandbox.

## Workload and validation boundary

`tests/input-pages/devtools-async.html` warms 1,024 generated functions, then executes 2,048 Promise steps with stack capture after attaching DevTools. It checks the computed result and records guest elapsed time. The original no-stack variant completed in 53.40 ms; the final stack-capture baseline completed in 333.10 ms on the diagnostic image. These different workloads must not be compared as an optimization result. The final baseline is `jit-metadata-stack-baseline-suite.json` and its accompanying measurements.

The host supervisor-policy test passed with AddressSanitizer and UndefinedBehaviorSanitizer (`jit-metadata-policy.log`). The native build, EFI link and binary validator subsequently passed. Image `cd4dc84c0c31d7dc9af303d9e17c95b92acc7c298f1d17af70cc0260bd5a2e28` passed all seven cases in `jit-metadata-final-suite.json`: inspector stack capture, workers, DevTools interaction, asynchronous scrolling, WebGL, the live portfolio site and WebGPU. All ran at CPL3 without unexpected renderer faults. The separate probe image passed boundary cases 1 through 14 and fault recovery in `jit-metadata-boundary-suite.json`.

The modified image completed the stack-capture workload in 94.60 ms. The 333.10 ms baseline ran under different host load; these measurements do not establish a controlled speedup. The live YouTube Console failure remains unresolved.

## Live-site memory failures

The `jit-metadata-youtube` run terminated inside QEMU's writable-directory FAT implementation with `get_cluster_count_for_direntry: Assertion 'mapping->mode & MODE_DELETED' failed`. This host emulator failure is separate from renderer failures. The input runner now checks QEMU process termination during its wait loops.

Fresh raw FAT disk runs reproduced renderer memory failures on both the previous diagnostic image and the modified image. `build/jit-metadata-raw-old/browser-serial.log` records `Fatal process out of memory: Worklist::Segment::Create`; `build/jit-metadata-raw-new/browser-serial.log` records `bad_alloc was thrown in -fno-exceptions mode`. Symbolization against the new run's matching payload traces the latter through `Capture::Image`, `SkPicture::serialize`, `CaptureScrollScene` and `FrameLoop::RecordScroll`. This identifies the failing allocation path, not the source of accumulated memory pressure.

The YouTube observer previously continued after renderer recovery in the older run. Its health check now rejects renderer termination for YouTube and both inspector scenarios, and recognizes V8's process-out-of-memory diagnostic. Intentional recovery scenarios remain permitted. Neither failed raw-disk run establishes successful playback or working Console input.

For failure diagnostics, `QEMU_TEST_FAILURE_BACKTRACE=1` together with `QEMU_TEST_GDB_PORT` and the matching `payload.elf` captures guest stacks, registers and the broker slot before teardown. `INPUT_POINTER_TRACE=1` records requested QMP click coordinates. These diagnostics do not change guest registers or inject page script.

The subsequent [page reclamation investigation](PAGE-RECLAMATION.md) separates fixed supervisor page-table exhaustion from the physical broker data quota. Bounded BSP table growth passes the sparse mapping and sandbox recovery tests, but the live site still reaches the 2 GiB broker limit before Inspector interaction can be validated.

## Sequential metadata-scope comparison

`metadata-controlled-results.json` records four sequential runs in baseline, modified, modified, baseline order. Each used four QEMU CPUs, 4 GiB guest memory, a fresh raw FAT profile, the same HTTPS fixture and the final stack-capture workload. No other QEMU test ran concurrently. The baseline is default image `94d711c0fe3942329da0313acd8512a46444cc1b60d5373c79111c64f021d978`; the metadata-scope image is `cd4dc84c0c31d7dc9af303d9e17c95b92acc7c298f1d17af70cc0260bd5a2e28`. This comparison excludes the later page-table growth changes.

The baseline recorded 115.10 and 113.60 ms; the modified image recorded 86.20 and 90.40 ms. The respective two-sample medians are 114.35 and 88.30 ms. All four runs passed the result assertion at CPL3 without unexpected renderer faults. These measurements support a reduction for this Inspector workload, not a general browser speedup or resolution of the live YouTube failure. Host load snapshots and individual suite records accompany the measurements.

The subsequent [grant revocation optimization](GRANT-REVOCATION.md) identifies a BSP page-table walk during ordinary heap release and records a controlled comparison plus sandbox and browser regressions.
