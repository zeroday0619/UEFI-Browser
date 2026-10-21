# Tabs and process monitor

Implemented and validated on 2026-10-19 using QEMU/OVMF with KVM. This adds multiple documents to the existing shared renderer process; it does not complete renderer-process isolation.

## Implementation

- `src/blink/browser_entry.cc` owns up to four tabs. Each tab has its own Blink frame, pixel storage, Skia surface, address editing state and stable identifier. The selected tab supplies page pixels to the native browser UI. New, select and close controls support mouse input and keyboard shortcuts.
- `src/blink/browser_frame.cc` retains history within each frame, assigns distinct widget identifiers and opens user-activated new-window requests in foreground tabs. Both the new-window callback and non-current navigation policies are handled. Opener/WindowProxy semantics remain unsupported.
- `src/blink/frame_loop.cc` stops pending paint/raster tasks for hidden tabs. Hidden documents retain their state; JavaScript and network operations are not forcibly suspended.
- `src/blink/resource_loader.cc` reference-counts frame owners so closing a tab does not uninstall the shared loader used by other tabs.
- `src/process/process.c` exposes a read-only snapshot of AP worker assignments, service state and counters. The native monitor displays these alongside tracked C heap usage and tab presentation counts.

The monitor refreshes when the main loop runs, with a 500 ms target. Its BSP fallback counter records idle/stopping AP-service handoffs, not every frame in the normal single-CPU BSP path. Heap accounting excludes virtual-memory commitments and AP page allocations. CPU percentages and per-tab memory attribution are unavailable. The monitor cannot terminate AP workers or interrupt a main-thread script.

Two integration defects were corrected during testing. A non-current Blink navigation policy was previously treated as a current-tab navigation. Separately, keyboard modifier state from Ctrl+Home persisted into mouse input. Keyboard modifiers are now scoped to each firmware keystroke; held-modifier mouse gestures are unsupported because physical key release is not tracked. The first-raster log marker now requires a completed Blink raster callback rather than a native toolbar presentation.

## Verified behavior

All listed runs used the final EFI image identified in `tabs-result.json` (local artifact).

| Run | Verified outcome | Evidence |
| --- | --- | --- |
| 4 CPUs, deterministic HTTP fixtures | Form values and JavaScript state survive switching; page pixels including scroll position match before/after switching; independent history navigation, background close, surviving subresource load, new-window links, four-tab limit, last-tab replacement and pending navigation close pass | Log (`tabs-4cpu.log`; local artifact) |
| 1 CPU, same fixtures | The same tab tests pass; the monitor reports zero AP workers and BSP presentation | Log (`tabs-1cpu.log`; local artifact), capture (`tabs-monitor-1cpu.png`; local artifact) |
| 4 CPUs, real HTTPS website | `zeroday0619.dev` loads and accepts click, Space and wheel input; returning from a settings tab preserves an interactive website; settings survives closing the website tab | Log (`tabs-site.log`; local artifact), website (`tabs-site.png`; local artifact), monitor (`tabs-monitor.png`; local artifact) |
| 4 CPUs, browser settings | Theme, font size, JavaScript and image preferences, search, startup choice, About and Back pass with the new tab strip | Log (`tabs-settings.log`; local artifact) |
| Static binary checks | Bounded ELF loads, no required unresolved symbols, no Linux syscall instructions and valid EFI structure | Log (`tabs-binary.log`; local artifact) |

All QEMU runs completed browser teardown and returned to firmware. The real-site and monitor captures were visually inspected. No physical-hardware validation or new performance comparison was performed.

## Reproduction

Run from the workspace root. The fixture server is needed for the `tabs` scenarios; run it in a separate terminal and stop it after testing.

```sh
python3 outputs/chromium-uefi/build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/outputs/chromium-uefi/build/blink" outputs/chromium-uefi/build-musl.sh
python3 outputs/chromium-uefi/tests/redirect-server.py
```

```sh
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=tabs python3 outputs/chromium-uefi/tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=1 INPUT_SCENARIO=tabs python3 outputs/chromium-uefi/tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=tabs-site python3 outputs/chromium-uefi/tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=settings python3 outputs/chromium-uefi/tests/browser-qemu.py
BUILD_DIRECTORY="$PWD/outputs/chromium-uefi/build/blink" python3 outputs/chromium-uefi/validate-binary.py
```

The QEMU scripts share the build directory and must run sequentially. They use fresh test firmware variables by default and do not change the runner's normal saved profile.
