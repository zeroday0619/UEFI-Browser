# Dedicated worker execution validation

The GPU availability statements below describe this report's recorded build. See [software GPU validation](SOFTWARE-GPU.md) for the subsequent ANGLE, Dawn and SwiftShader implementation.

## Implemented path

Blink creates real DedicatedWorker global scopes and V8 isolates through `src/blink/workers.cc`. The adapter supplies a worker host and per-worker resource-loader factory. The existing HTTP(S) loader applies its request validation, redirect handling and CORS checks to worker requests. Synchronous worker imports use the same loader with a bounded wait and the worker termination event. Normal document synchronous loading remains outside this change.

`src/musl/threads.c` supplies cooperative pthread creation, joining, detaching and futex wait/wake/requeue operations. Each context has its own stack, ELF TLS image, musl thread descriptor, thread-specific values, errno and C++ TLS destructor list. The context switch saves callee-preserved registers, FS base and x87/SSE floating-point state. Main-thread message-pump iterations and blocking waits yield to runnable contexts. These are real separate execution contexts in the same renderer address space; they are not CPU-parallel or preemptive OS threads.

The runtime has ten context slots including the main context. Worker creation keeps one slot in reserve. With no other background contexts active, eight workers can execute at once. Excess creation reports an asynchronous worker error so handlers installed after construction receive it. Thread exit runs cleanup and thread-local destructors. Joined or detached completed contexts release their TLS and stacks for reuse.

The V8 code-range allocator previously committed 512 MiB per isolate and then used MADV_DONTNEED. This firmware cannot discard readable backing without a demand-fault handler, so each range retained physical memory. A simultaneous-worker test exhausted the 4 GiB VM. The UEFI-specific V8 patch now allocates backing when individual code pages become accessible. It retains the virtual range and V8 permission transitions while avoiding whole-range physical commitment. The previously failing capacity test passes with eight active workers and twelve rejected requests.

Breakpoint exceptions now enter the AP renderer's existing failure-recovery path instead of the firmware debug exception handler. This is crash recovery, not a security sandbox.

## Evidence

Image SHA-256: `d309105da5fed67146648dff8b70f124b8c2af668d5e9de6627794c10a7d2077`.

The final image passed ten four-vCPU scenarios and two one-vCPU scenarios. Both configurations passed all 15 worker assertions and the three lifecycle/capacity assertions. The recovery scenario observed exception vectors 2, 3, 6 and 14 and restarted the renderer after each stop. All five fault-recovery allocation counts matched; normal shutdown subsequently reported zero remaining broker allocations. Every scenario reached EFI payload, stack and TLS release. The binary validator found no required unresolved symbols or syscall instructions.

The page tests run through QEMU input devices against local HTTPS fixtures. They use native Worker APIs and do not replace Worker with a JavaScript shim.

- `workers`: 15 assertions for classic script execution, absence of `document`, structured messages, transferable ArrayBuffer detachment and binary round trip, worker fetch, timers, importScripts, nested workers, close, recreation, module imports, cross-origin constructor rejection and invalid script MIME rejection.
- `workers-limits`: 20 simultaneous creation requests, asynchronous capacity errors, termination and capacity recovery, and suppression of messages after termination before startup. Three page assertions cover these outcomes.
- Native startup probes: separate TLS and errno, thread-specific destructors, join results, condition wake/broadcast, timed waiting and independent floating-point rounding/exception state. The four-vCPU configuration runs these probes in both browser and AP renderer runtimes.
- `renderer-recovery`: controlled GDB fault injection for page faults, invalid instructions and breakpoints, plus NMI termination of an infinite JavaScript task. The scenario verifies restart, page interaction and broker allocation reclamation.
- Regression scenarios: PCM audio including captured waveform analysis, WebM video, IndexedDB, Cache Storage, input during long JavaScript, tab/site interaction and service probes. A successful service-probe scenario does not imply that unavailable GPU APIs work.

Reports and logs:

- [Four-vCPU suite](worker-final-suite.json).
- [One-vCPU suite](worker-final-single-suite.json).
- [Service availability](worker-final-service-capabilities-service-capabilities-results.json).
- [Binary validation](worker-final-binary.log) and [static checks](worker-final-static-checks.json).
- [Build log](worker-final-build.log).
- Earlier failing capacity evidence: [renderer OOM](worker-lifecycle-workers-limits.log). Later successful reproduction: [capacity recovery](worker-memory-fixed-workers-limits.log).

## Remaining limits

A synchronous JavaScript task that never yields can starve other contexts in its renderer. Worker termination cannot preempt such a task through this cooperative runtime. The separate AP renderer can still be terminated by the browser's existing supervisor; that discards all documents and workers in the renderer. Single-vCPU execution lacks that independent AP supervisor. No claim of low input latency during arbitrary worker computation is made.

Blob/data worker scripts, SharedWorker, ServiceWorker, worker storage/socket brokers, general pthread cancellation, robust/process-shared mutexes, caller-provided stacks and full pthread/Web Platform Test conformance remain unsupported or unverified. HTTP(S) classic/module workers are the validated scope. `importScripts` waits for at most 30 seconds or the smaller supplied timeout and stops when the worker termination event is signaled.

WebGL and WebGPU still have no working context/device providers. ANGLE/Dawn library presence does not establish graphics execution. The renderer and its worker contexts still execute at CPL0 with firmware identity mappings. They have no hostile-code security sandbox or site isolation. Web-origin and loader checks are separate from hardware-enforced process isolation.

## Reproduction

Start the local fixture server in a separate terminal:

```sh
python3 tests/network-servers.py
```

```sh
python3 build-blink-libraries.py --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 tests/state-suite.py --https-ca build/network-https/ca.esl --prefix worker-final workers workers-limits service-capabilities audio media indexeddb cache scroll-input tabs-site renderer-recovery
python3 tests/state-suite.py --workers 1 --cpus 1 --https-ca build/network-https/ca.esl --prefix worker-final-single workers workers-limits
```

The recovery scenario requires GDB and automatically selects a local QEMU debug port. Stop the fixture server after testing. These results cover QEMU/KVM with OVMF, 4 GiB RAM, Chromium 158.0.8110.0 and musl 1.2.5; physical hardware is unverified.
