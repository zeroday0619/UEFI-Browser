# Asynchronous wheel scrolling and renderer recovery

Validation date: 2026-10-19. The BSP now composes eligible page scrolling while the AP executes an uninterrupted JavaScript task. A loader supervisor can interrupt the AP and restart the renderer after selected CPU exceptions. This report supersedes the scroll and termination limitations in [RENDERER-PROCESS.md](RENDERER-PROCESS.md).

The root-only cache design and its scrolling limitations below are historical. The scene compositor described in [SCROLL-TREE.md](SCROLL-TREE.md) supersedes them; the recovery mechanisms remain in use.

## Implementation

`src/blink/frame_loop.cc` records an expanded Blink paint artifact, classifies chunks by ancestry of the root scroll transform, and preserves their paint order in up to four scrolling/fixed groups. Each group is rasterized in bounded-height tasks. The snapshot covers three viewport heights. Unsupported property trees fall back to normal Blink event dispatch. The ordinary frame remains available independently of the cache.

`src/blink/remote_frame.cc` publishes completed pixels and immutable scroll layers through the existing release/acquire channel. The BSP copies the layers into its own Skia images before acknowledging the publication. It never calls Blink or reads mutable AP paint objects during composition. Wheel input updates the BSP's visual offset and queues an absolute scroll position with a monotonically increasing sequence. The AP applies that position after JavaScript returns. A completed raster carries the sequence captured when its paint record was made; older frames cannot undo newer speculative scrolling. Navigation and renderer recovery invalidate the cache. Applying an absolute scroll cancels any older raster task because that frame cannot acknowledge the new offset; this avoids delaying the next scroll event behind unusable paint work.

`src/loader-renderer.h` installs AP-local handlers for NMI, divide error, invalid opcode, general protection and page fault. An NMI IPI interrupts the renderer even with maskable interrupts disabled. The handler records the vector, error code and instruction address, returns through `iretq`, and transfers control to a saved loader context. The loader restores CR3, FS base and the firmware IDT. A completion/release handshake keeps the private IDT installed until any requested NMI has been acknowledged. The BSP also waits for the MP completion event before reclaiming the AP stack and page tables.

`src/process/renderer_runtime.c` records successful AP pool/page allocations in BSP-owned bookkeeping. After AP quiescence, it releases remaining allocations without trusting the interrupted AP allocator's data structures. `src/network/async.c` independently tracks renderer job handles and transferred response storage. Interrupted requests are abandoned; active firmware calls retain their stacks until completion. A fresh runtime, TLS image and page-table root replace the terminated process. Channel generations prevent previous documents from publishing into recovered tabs.

The native process monitor provides **End renderer** and the **Delete** shortcut. Recovery retains native tabs and address text, displays an explanation, and discards all documents, JavaScript state and per-tab session histories. Reload or navigate to create a new document. Browser shutdown allows two seconds for graceful return before requesting an NMI. The single-CPU path remains a local renderer and cannot use independent force termination.

## Long-task scroll verification

The controlled fixture runs one uninterrupted JavaScript task for six seconds. QMP sends wheel input after the begin marker and captures the framebuffer before the end marker. The final tested image showed the first scroll in **17.18 ms**. This is one QMP-to-capture sample, including host and screenshot overhead, not optical latency or a percentile.

Thirteen wheel ticks moved the page **1,040 pixels** while the script was still running. A colored marker initially outside the viewport became visible. Fixed buttons and the input field were pixel-identical in their comparison region. No DOM scroll callback ran inside the long task. After the script returned, Blink reported offset 1,040 and acknowledged sequence 13. The asynchronous and reconciled page captures had no color-channel differences greater than two in the checked region `(25,180)-(1000,700)`.

Evidence: [serial log](renderer-release-renderer-scroll.log), [measurements](renderer-release-scroll-results.json), [before](renderer-release-input-scroll-before.png), [during JavaScript](renderer-release-input-scroll-offscreen-during-javascript.png), [after reconciliation](renderer-release-input-scroll-reconciled.png). The captures were visually inspected.

## Termination and fault verification

The recovery scenario keeps two native tabs open and performs four interruptions in one QEMU session:

1. Infinite JavaScript with a pending network request, terminated through the monitor's Delete shortcut.
2. An instruction-fetch page fault injected into the AP through GDB at an unmapped address.
3. A six-second JavaScript task interrupted through the native **End renderer** button before its completion marker.
4. An invalid-opcode exception injected at the AP runtime's `ud2` instruction.

All four interruptions returned through the supervisor, restarted the AP, and permitted loading/reloading the fixture and activating its page button. The fault vectors were 2, 14, 2 and 6. Two interrupted sessions had active network requests. The broker reclaimed 594, 593, 597 and 593 outstanding allocations respectively. Both native tabs remained selectable after recovery. The tracked allocation count matched the number reclaimed on all four interruptions. The final normal renderer shutdown reported zero reclaimed and zero tracked allocations and returned to firmware. This confirms the exercised ownership paths; it is not a general memory-corruption containment guarantee.

The first termination took **92.84 ms** from QMP Delete submission to the recovery-launch log, including AP quiescence, resource cleanup and dispatch of the fresh AP. It does not measure completion of the new renderer's initialization or page load. GDB stops were used only for fault injection, not the scroll latency measurement.

Evidence: [serial log](renderer-release-renderer-recovery.log), [native recovery message](renderer-release-input-renderer-recovery-message.png), [page after recovery](renderer-release-input-renderer-recovered.png).

## Repeated layout workload

The 800-card fixture repeatedly runs JavaScript and changes grid width. An initial implementation completed the previous raster even after a newer scroll offset made that frame unusable. Cancelling that work reduced the observed DOM scroll-event delay in a five-sample comparison:

| QMP-to-DOM event observation | Before cancellation, median / maximum | After cancellation, median / maximum |
| --- | --- | --- |
| Keyboard | 5.75 / 56.97 ms | 5.65 / 51.77 ms |
| Click | 52.10 / 77.58 ms | 41.65 / 51.98 ms |
| Scroll | 52.65 / 125.30 ms | 16.50 / 67.63 ms |

These observations include host polling and, for clicks, the test's button hold. They are distinct from framebuffer latency and do not establish a general speedup or worst-case bound. Recording and rasterizing the larger cache still adds rendering work. Evidence: [comparison](renderer-async-heavy-comparison.json), [final samples](renderer-async-heavy-samples.json), [serial log](renderer-async-heavy-fixture.log).

## Regression coverage

Nine QEMU scenarios passed on the [regression image](renderer-async-regression-image-sha256.txt): asynchronous scroll, repeated CPU/layout work, two-tab renderer recovery, F12 shutdown during infinite JavaScript, tab/history lifecycle, the live `https://zeroday0619.dev` site, native and queued page input during long JavaScript, cancelled/nested wheel input, and single-CPU fallback. All returned to firmware. The live-site test exercised HTTPS/subresources, theme activation by mouse and keyboard, scrolling, settings and tab restoration. Unsupported site scroll properties used the renderer path.

The last relink only extended the allocation-reclamation diagnostic with its tracked count. Both asynchronous scrolling and the four-interruption recovery scenario passed again on that final image, including equality checks between tracked and reclaimed allocations. The [aggregate evidence](renderer-async-results.json) records both image hashes and their individual test results. [Static and host checks](renderer-async-static-checks.json) passed. The captures of scrolling, recovery and the live site were inspected.

## Reproduction

The final build uses Chromium 158.0.8110.0, musl 1.2.5, Clang 23.1.3, QEMU 11.1.2/KVM and OVMF 2026.08+ds-2 with 4 GiB RAM. The AP scenarios use four vCPUs. [Image SHA-256](renderer-async-image-sha256.txt), [binary checks](renderer-async-binary.log) and [final scenario results](renderer-release-suite.json) identify the delivered image and its final reruns. The [full regression scenarios](renderer-async-suite.json) refer to the preceding image with the same rendering and lifecycle logic.

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
bash tests/pump-loop.sh
bash tests/network-scheduler.sh
python3 -m py_compile tests/browser-qemu.py tests/input-scenario.py tests/redirect-server.py
```

Run `python3 tests/redirect-server.py` in another terminal. Run QEMU cases sequentially because they share the ESP, log and firmware-variable image. Do not relink the EFI image while QEMU is running.

```sh
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=renderer-scroll python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 QEMU_TEST_GDB_PORT=12345 INPUT_SCENARIO=renderer-recovery python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=renderer-exit python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=tabs python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=tabs-site python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=heavy-fixture python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=renderer-blocking python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=wheel-fixture python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=1 python3 tests/browser-qemu.py
```

The recovery scenario requires GDB and uses the QEMU debugging port bound to loopback. It does not expose a page-accessible crash command. `renderer-recovery-first.log` and `renderer-scroll.log` describe intermediate images; the regression suite uses `renderer-async-` logs and `renderer-final-` captures, while delivered-image reruns use the `renderer-release-` prefix.

## Remaining boundaries

- This compositor supports vertical root-frame wheel scrolling only. Nested scroll containers, sticky constraints, wheel listeners, non-root effects, horizontal offsets, repaint-dependent backgrounds and more than four paint groups use renderer dispatch.
- Cached content moves only within the three-viewport raster window. Beyond that window, visual motion waits for new renderer pixels. Scrollbar thumb painting, keyboard scrolling, scrollbar dragging, scroll callbacks and layout changes still wait for the AP.
- The cache is a snapshot. Page animations and newly changed DOM content cannot update while the renderer task is blocked. Click handlers and text editing retain JavaScript run-to-completion semantics.
- All tabs share one AP renderer. Termination discards all tab documents and their session histories. Native tabs and addresses survive; no unsaved form or JavaScript state is restored.
- The AP retains CPL0 privileges and firmware mappings. The supervisor requires intact loader state and a usable exception stack. Stack destruction, double faults, machine checks, firmware faults and arbitrary memory corruption are not contained. This is not Chromium's sandbox.
- Physical firmware, x2APIC operation, restart allocation failure and independent human review have not been validated by these QEMU runs.
