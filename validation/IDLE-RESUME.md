# Idle completion and presentation fallback

The full renderer-process split is not implemented. Blink DOM, V8 JavaScript, layout, Skia rasterization and HTTP still execute in the original main process. Only completed-frame transfer uses the existing isolated AP workers.

## Changes

`src/process/process.c` polls the MP completion event once per input-loop iteration instead of waiting with repeated Stall calls. While workers are stopping or stopped, presentation returns immediately to the existing BSP GOP path. It never restarts APs while the cursor lock is held. Four successive frames with inter-frame gaps below 100 ms request restart from the input loop. Disabled workers also finish the asynchronous stop path; they are never restarted. Final shutdown still joins all active workers before releasing memory.

`tests/process-qemu.py` verifies that the short settings navigation uses the fallback without an AP restart. It continues checking distinct CR3 roots, CPL3 protection faults, concurrent execution, pixel equality across 1/2/4 CPUs and descriptor restoration before release.

## Settings comparison

Each run contains four full presentations. Values are cumulative microseconds inside `BrowserContext::Present`, including toolbar drawing and GOP transfer. These are single-run observations, not input latency, optical latency or statistically stable benchmarks.

| QEMU CPUs | Previous implementation, us | Current implementation, us |
| --- | ---: | ---: |
| 1 | 2307 | 2513 |
| 2 | 14539 | 3083 |
| 4 | 13155 | 3061 |

All three final-image KVM runs passed and produced identical settings pixels. The multiprocess runs used one startup and one idle stop, with three BSP fallback frames. See `process-comparison.json` and `process-{1,2,4}cpu.log`.

## Remaining renderer boundary

- `src/loader.c` loads one fixed-address executable and one TLS block. There is no child ELF loader with private writable segments and child TLS.
- `src/musl/allocator.c` calls firmware allocation directly and owns one global allocation list.
- `src/virtual_memory.c` performs privileged CR3 and TLB operations and calls firmware page allocation.
- `src/posix.c` obtains firmware time, RNG, memory maps and protocol services directly.
- `src/musl/startup.c` and `syscalls.c` access serial ports directly; pthread creation is unsupported.
- `src/blink/input.cc`, browser settings and browser entry own firmware input, NVRAM, timers and GOP access.
- `src/network/async.c` switches cooperative stacks within the main process. Resource jobs carry native pointers and callbacks, not serialized interprocess messages.
- `src/process/entry.S` runs a fixed copy program. Its return trap does not resume a general child after a brokered service call.

These call sites require a process-local runtime and validated broker protocol before the complete Blink engine can execute at CPL3. Calling the existing runtime on an AP is not a renderer-process implementation. Firmware MP startup can still delay the input-loop pass after a sustained burst. Long JavaScript and layout tasks remain main-thread blockers.

## Final-image website verification

Four-CPU QEMU/KVM loaded `https://zeroday0619.dev` with visible Korean text and imagery. Loading-time input was exercised. Post-load theme click, keyboard Space activation and wheel scrolling passed. The screenshot was visually inspected: website capture (`idle-site.png`; local artifact). This does not establish complete site fidelity.

The run presented 400 frames: 330 through three AP workers and 70 through BSP fallback. There were 16 service starts, 16 idle stops and zero worker failures. Theme change was observed after 310 ms for click and 62 ms for Space; these values include test-harness event delays and polling. They are not optical latency measurements or a controlled comparison with the old implementation.

Validation commands:

```sh
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/outputs/chromium-uefi/build/blink" outputs/chromium-uefi/build-musl.sh
python3 outputs/chromium-uefi/tests/process-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=heavy-site python3 outputs/chromium-uefi/tests/browser-qemu.py
BUILD_DIRECTORY="$PWD/outputs/chromium-uefi/build/blink" python3 outputs/chromium-uefi/validate-binary.py
```

The build, CPU comparison, website test, clean firmware return and static binary validation passed. See website log (`idle-site.log`; local artifact), binary validation (`idle-binary.log`; local artifact) and machine-readable results (`idle-result.json`; local artifact).

Two-CPU QEMU/TCG settings navigation and firmware return also passed on the same final image. The log records one startup, one idle stop, three fallback frames and zero worker failures: TCG log (`idle-tcg.log`; local artifact).
