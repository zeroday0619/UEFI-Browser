# AP renderer and browser service separation

This report describes the earlier renderer-separation build. [Scroll and recovery validation](SCROLL-RECOVERY.md) supersedes its asynchronous-scroll and termination limitations.

Validation date: 2026-10-19. This change moves actual Blink frames, V8 execution, layout and Skia page rasterization into an AP address space. It does not provide a Chromium-equivalent security sandbox or an asynchronous scroll compositor.

## Execution ownership

| Component | Owner |
| --- | --- |
| Native address bar, tab strip, process monitor, input collection and GOP presentation | BSP browser runtime |
| Blink DOM, page JavaScript, lifecycle/layout, paint recording and Skia page rasterization | One AP renderer shared by all tabs |
| Writable ELF image, musl/libc++ globals, allocator state, thread-local storage, stack and virtual-memory reservations | Separate instances in browser and renderer |
| Firmware memory services, clock calibration, entropy and preference variables | BSP service broker |
| HTTP/HTTPS, TLS negotiation/certificate checking and cooperative request scheduling | BSP firmware network service |
| Permission-denial Mojo endpoint | Local endpoint in each renderer frame |

The loader uses nonblocking `StartupThisAP`, creates private backing for the payload's writable segment, remaps it in a private page-table root, and initializes a fresh TLS template and stack. The AP restores its original CR3 and FS base before returning to MP Services. The BSP waits for both application completion and the MP completion event before releasing the AP's allocations. EFI calls stay on the BSP; the pinned [EDK II MP Services contract](https://github.com/tianocore/edk2/blob/edk2-stable202608/MdePkg/Include/Protocol/MpService.h) restricts AP firmware-service calls unless specifically permitted.

An initialized-data probe and a TLS probe start with their ELF initial values in the child even after the BSP has changed its own copies. Child writes do not alter the BSP probes. In the final blocking test:

| CPU | CR3 | FS base |
| --- | --- | --- |
| BSP | `0x7da7d000` | `0x7de82000` |
| Renderer AP | `0x5b8e1000` | `0x5c134000` |

The earlier debugger capture in `renderer-process-stacks.log` (local artifact) independently shows CPU 1 executing generated code while CPU 0 remains in firmware/browser work, with different CR3 and FS base values. Debugger stops were not used in the final latency measurement.

## Input, frames and lifetime

A single-producer/single-consumer command ring carries frame identifiers, generations, navigation sequence numbers and input samples. Release/acquire publication orders both commands and completed pixels. The producer never overwrites an unacknowledged frame. Old navigation publications cannot replace newer native navigation state.

The native UI continues consuming input when the renderer command ring is full. Page input reserves 32 ring slots for controls and can defer up to 8,192 samples per tab. Consecutive ordinary pointer-motion samples coalesce; button transitions remain distinct. Overflow is counted. Navigation clears deferred input from the previous document. A close request marks the slot for destruction, and the browser only reuses it after renderer acknowledgement.

Small renderer allocations reuse private slabs. Virtual-memory backing pages come from 1 MiB batches instead of one broker request per 4 KiB page. The six-second fixture required 523 page-allocation service calls in the final build, compared with 134,275 before batching. These are service-call counts, not a controlled end-to-end speedup estimate. Free physical batches remain cached until renderer shutdown and therefore retain peak physical commitments during the session.

## Six-second JavaScript task

`tests/input-pages/renderer-blocking.html` starts a delayed network response and then runs one uninterrupted JavaScript task for six seconds. QMP clicks the native address field and types `x` while that task is active. A cropped address-field framebuffer comparison observes the native update before the JavaScript completion marker.

The final run measured **17.37 ms** from QMP key submission to the first changed address-field capture. This single sample includes QMP and screenshot overhead; it is not optical latency, a percentile or a response-time guarantee. The source image was inspected: address field during JavaScript (`renderer-address-during-javascript.png`; local artifact).

The network-service completion marker appears between `ISOLATION begin` and `ISOLATION end`. Page clicks, input text `y` and wheel scrolling are retained and delivered after the task returns. They do **not** execute during the task. The AP task-gap measurement is approximately six seconds; this is distinct from the BSP native-input polling interval.

Evidence: log (`renderer-process-blocking.log`; local artifact), structured result (`renderer-process-blocking.json`; local artifact), page after JavaScript (`renderer-after-javascript.png`; local artifact).

## Regression coverage

The firmware build and EFI link completed with the pinned Chromium 158.0.8110.0 sources. Both the BSP and AP execute the existing 46 musl runtime checks, sparse 8 GiB reservation/protection checks, entropy and clock checks, and compatibility probes. Both release their heaps and mappings before EFI return.

The final image was exercised with QEMU 11.1.2/KVM, OVMF 2026.08+ds-2, 4 GiB RAM and four vCPUs; the single-vCPU case checks local fallback. All QEMU runs are sequential because they share the ESP and test firmware-variable image. All five final QEMU scenarios passed and returned cleanly to firmware. Binary validation, the message-pump tests, EEVDF/coroutine/SSE2 tests, Python compilation, shell syntax and whitespace checks passed. Clang 23.1.3 also accepted the new runtime and loader with `-Wall -Wextra -Werror` syntax checks.

| Scenario | Observed result | Evidence |
| --- | --- | --- |
| Six-second task | Native address paint observed in 17.37 ms; network completed before the task returned; page input dispatched afterward | Blocking log (`renderer-process-blocking.log`; local artifact) |
| Tabs | Form/DOM state, history, reload, target-blank, closing/loading lifetime and tab limits passed | Tabs log (`renderer-process-tabs.log`; local artifact) |
| 800-element repeated computation/layout | Five samples each: keyboard median/max 10.95/46.38 ms; click 41.57/46.64 ms; wheel 16.17/21.32 ms | Heavy log (`renderer-process-heavy-fixture.log`; local artifact), samples (`renderer-process-heavy.json`; local artifact) |
| `https://zeroday0619.dev` | HTTPS document/subresources, loading input, theme click/Space, wheel, settings and tab restoration passed | Site log (`renderer-process-tabs-site.log`; local artifact), loading samples (`renderer-process-site-samples.json`; local artifact) |
| One vCPU | Local renderer fallback, startup and clean teardown passed | Fallback log (`renderer-process-1cpu.log`; local artifact) |

Heavy-fixture measurements are host QMP-to-DOM-log observations with 5 ms polling; clicks include a 30 ms button hold. They are not directly comparable with the native address-field screenshot measurement. The live site's visible theme change took 89.78 ms for click and 55.39 ms for Space in this run. The renderer still recorded a 369.28 ms task gap during site loading. No native polling gaps above the logger's 50 ms threshold were recorded in these scenarios; that is not a worst-case guarantee.

The aggregate results (`renderer-process-results.json`; local artifact), binary validation (`renderer-process-binary.log`; local artifact), and EFI SHA-256 (`renderer-process-image-sha256.txt`; local artifact) identify the final tested build. The final website capture (`renderer-process-heavy-loaded.png`; local artifact), process monitor (`renderer-process-tabs-site-monitor.png`; local artifact), and blocking-task captures were inspected. Blink's own `maximum_queue_us` begins after an IPC command reaches its input dispatcher and therefore excludes the time spent waiting for a long task in the transport queue. Earlier `*-first` logs and the debugger capture describe intermediate builds and are not the final image's performance results.

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
bash tests/pump-loop.sh
bash tests/network-scheduler.sh
python3 -m py_compile tests/browser-qemu.py tests/input-scenario.py tests/redirect-server.py
```

Start `python3 tests/redirect-server.py` in a separate terminal for fixture cases:

```sh
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=renderer-blocking python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=tabs python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=heavy-fixture python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=tabs-site python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=1 python3 tests/browser-qemu.py
```

## Remaining boundaries

- The renderer executes at CPL0 and retains firmware identity mappings. Broker requests use trusted pointers. This is execution/state separation, not a fault-containment or security boundary.
- There is one renderer for all tabs. A synchronous script blocks page event handlers, text editing, layout and scroll dispatch in that renderer. Native UI and networking remain separate; asynchronous page scrolling still requires a compositor path.
- Runtime and network services share the BSP browser process. They are separated from the renderer, not placed in a third independent process. Long firmware operations can still delay native input between cooperative yields.
- Infinite scripts and renderer faults cannot yet be forcefully terminated or restarted safely. Shutdown waits for the current renderer task.
- AP idle waits use `pause`; interrupt-driven sleep and a general process scheduler are not implemented.
- Physical hardware and independent human review remain outstanding.
