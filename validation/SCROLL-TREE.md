# Nested asynchronous scrolling and on-demand raster tiles

This report records the earlier vertical-input image. The subsequent [two-axis scrollbar implementation and validation](SCROLL-CONTROLS.md) supersedes its custom-scrollbar, horizontal-offset and track/button limitations.

Validation date: 2026-10-19. The subsequent [asynchronous input implementation](SCROLL-INPUT.md) extends the wheel-listener, keyboard and scrollbar boundaries recorded here. This implementation replaces the root-only raster snapshot described in [SCROLL-RECOVERY.md](SCROLL-RECOVERY.md). It retains the AP renderer, sequence acknowledgements and supervisor recovery protocol.

## Implementation

`src/blink/scroll_scene.cc` captures the existing Blink layout with infinite cull rectangles. Every paint layer is invalidated before capture because an unchanged root cull rectangle otherwise prevents descendant updates. The previous runtime feature setting is restored and cull rectangles are invalidated again for subsequent normal painting. Capture does not force lazy content or new DOM work to complete.

Paint chunks retain their original order. Adjacent chunks with identical transform, clip and effect properties share a serialized SkPicture. The publication includes copied scroll IDs, scroll-parent IDs, bounds, rounded clips, opacity/blend groups and Chromium `StickyPositionConstraint` values. Images are serialized as raster pixels and fonts include their data. Resource IDs deduplicate images and fonts within the scene. The AP also reuses immutable serialized fonts and raster images from its previous scene, avoiding repeated full-font serialization. The BSP deserializes independent Skia objects before releasing publication ownership; it does not render from AP-owned Skia objects or access mutable Blink state.

The BSP computes the target scroll chain from the visible picture and clip ancestry. Scroll-parent IDs handle fixed-position overflow containers whose transform parent differs from their scroll parent. Vertical offsets are clamped to each container's bounds. Overscroll containment stops propagation. Sticky offsets use Chromium's constraint calculation, including ancestor sticky-box and containing-block offsets. The AP applies the identified scroll area's absolute offset with `ScrollType::kCompositor` when its task returns. Sequence acknowledgements reject stale frames.

Missing 512-by-256 pixel tiles are rasterized from the independent picture on the BSP. Edge tiles are cropped to picture bounds. The LRU retains at most 32 MiB of pixel storage and 1,024 entries. Deserialized pictures receive a Skia R-tree so tile misses skip unrelated offscreen drawing operations. Scrolling recorded content therefore does not depend on the old three-viewport raster extent. Matching serialized pictures retain their tiles across acknowledgements, even when another picture such as a scrollbar changes. Scenes are limited to 4,096 picture groups and 32 MiB of serialized picture/resource data. Oversized or unsupported scenes use ordinary renderer dispatch.

The ordinary viewport frame remains available independently of the scene. Navigation, tab destruction and renderer recovery discard scene resources. The IPC channel continues to use trusted pointers and CPL0 firmware mappings; serialization does not create a security sandbox.

## Entropy regression found during website testing

The UEFI specification website returned HTTP 403 and a Cloudflare challenge. Its JavaScript triggered a BSP page fault in firmware. A separate page containing `crypto.getRandomValues(new Uint8Array(65536))` reproduced the same fault. The entropy broker had passed an AP-only virtual buffer directly to a firmware driver executing with the BSP page table.

`src/process/renderer_runtime.c` now obtains entropy in a 256-byte shared stack buffer and copies each block into the destination from the AP. `src/posix.c` also exercises entropy in a private virtual mapping during the runtime probes. The controlled JavaScript case passed after the fix. The challenge page is not counted as a successfully loaded specification document. Evidence: before (`scroll-entropy-before.log`; local artifact) and after (`scroll-entropy-after.log`; local artifact).

## QEMU evidence

The controlled page runs an uninterrupted 12-second JavaScript task. QMP wheel input moves its nested container by 4,400 pixels and its root document by 5,120 pixels before the task ends. Both sticky headers remain attached to their scrollports, and distant colored markers become visible. A separate boundary case propagates a wheel tick from the fixed overflow container to the root; the containment case keeps the root stationary. The test compares the composed frame with Blink's subsequent reconciled frame.

The live website is [Python's Event Loop documentation](https://docs.python.org/3/library/asyncio-eventloop.html). The test requires HTTP 200 and distinct scroll element IDs for the sidebar and the document, rather than treating a sidebar wheel that bubbles to the root as independent scrolling. The recorded run used element 2628 for the sidebar and 644 for the document. Twenty-four document wheel ticks reached 1,920 pixels while the sticky sidebar remained visible. QMP framebuffer captures were inspected. This real-site case does not inject a long JavaScript task; the separate controlled case verifies that condition.

The earlier UEFI specification request returned a challenge instead of the document. The shorter Python asyncio pages did not require independent sidebar scrolling. Neither result is counted as a nested-scrolling website success.

### Delivered-image results

The final EFI image passed both primary QEMU scenarios. Its SHA-256 is `7bd65421728e1c2213f849a7f74c26f5842a403ab33ed69a3def4e8ab7090657`. The machine-readable results (`scroll-results.json`; local artifact), scenario exit statuses (`scroll-delivered-suite.json`; local artifact) and binary validation log (`scroll-delivered-binary.log`; local artifact) identify the delivered build.

| Check | Observed result |
| --- | --- |
| Uninterrupted JavaScript | 12,000 ms; nested scroll 4,400 px and document scroll 5,120 px before completion |
| Sticky positioning | Root and nested headers remained attached, including Korean glyphs |
| First wheel to framebuffer capture | 27.19 ms in this run; includes QMP and host capture overhead |
| Reconciliation | Exact pixel agreement in framebuffer rectangle `(300,80)` to `(1100,700)`; maximum channel difference 0 |
| Boundary propagation | Root moved 80 px with automatic chaining; containment prevented propagation |
| On-demand cache | 111 tiles created; maximum 87 resident tiles and 33,550,344 retained pixel bytes, below the 33,554,432-byte limit |
| Live website | Python Event Loop documentation returned HTTP 200; sidebar and root used distinct scroll IDs |
| Binary checks | Static x86-64 EFI image, bounded ELF loads, no required unresolved symbols or OS syscall instructions |

Screenshots: nested scrolling during JavaScript (`scroll-delivered-scroll-tree-input-tree-nested-busy.png`; local artifact), both distant markers and sticky headers (`scroll-delivered-scroll-tree-input-tree-root-busy.png`; local artifact), reconciled frame (`scroll-delivered-scroll-tree-input-tree-reconciled.png`; local artifact), live sidebar (`scroll-delivered-scroll-site-input-scroll-site-sidebar.png`; local artifact) and live document (`scroll-delivered-scroll-site-input-scroll-site-document.png`; local artifact). Serial logs: controlled page (`scroll-delivered-scroll-tree.log`; local artifact) and live website (`scroll-delivered-scroll-site.log`; local artifact).

The Korean-header fixture exposed repeated serialization of a large fallback font. Before AP resource reuse, 13 scene-recording samples exceeded 50 ms, ranging from 215,391 to 242,471 microseconds. The final run recorded one such sample at 218,571 microseconds during cold preparation and none thereafter. This comparison uses the same fixture and logged recording stage; it is not a general frame-latency benchmark.

### Regression coverage and build boundaries

The six-scenario regression suite (`scroll-final-suite.json`; local artifact) passed controlled and live scrolling, the earlier long-task scroll test, cancelable wheel events, live-site tabs, and forced renderer recovery. Recovery exercised NMI, page-fault and invalid-opcode paths with matching tracked/reclaimed allocations. The subsequent lifecycle suite (`scroll-release-suite.json`; local artifact) passed scrolling, live-site tabs, 65,536-byte JavaScript entropy generation and the single-CPU wheel fallback after explicit publication cleanup. The resource-reuse suite (`scroll-resource-suite.json`; local artifact) repeated both primary scrolling scenarios after font/image reuse was added. The final image adds the strict 4,096-group guard and repeats both primary scenarios again.

These image hashes and stages are recorded separately; the complete regression suite was not rerun on the final image. Host pump-loop sanitizer tests, EEVDF admission/fairness/overrun tests, coroutine cancellation and cleanup tests, and 4,128 SIMD cases passed. Python syntax checks, JavaScript fixture syntax checks and `git diff --check` also passed.

Build and verification commands run from the project directory:

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 tests/redirect-server.py
# Run QEMU scenarios sequentially in another terminal while the fixture server is active.
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=scroll-tree python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=scroll-site python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=renderer-scroll python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=wheel-fixture python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=tabs-site python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 QEMU_TEST_GDB_PORT=12345 INPUT_SCENARIO=renderer-recovery python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=entropy python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=1 INPUT_SCENARIO=wheel-fixture python3 tests/browser-qemu.py
bash tests/pump-loop.sh
bash tests/network-scheduler.sh
python3 -m py_compile tests/browser-qemu.py tests/input-scenario.py tests/redirect-server.py
git diff --check
```

Environment: Chromium 158.0.8110.0 (`f12c350cb1e1e7ea05c11d6045b0a04158ec9e28`), Clang 23.1.3, QEMU 11.1.2 with KVM, OVMF 2026.08+ds-2, four vCPUs, 4 GiB RAM and a 1280-by-800 framebuffer. The page viewport is 1280 by 720. Timing samples include host input and capture overhead and do not measure physical display latency.

## Changed implementation files

| Files | Responsibility |
| --- | --- |
| `src/blink/scroll_scene.cc`, `scroll_scene.h`, `scroll_snapshot.h` | Scene capture, serialization, property trees, sticky calculation and bounded tile cache |
| `src/blink/frame_loop.cc`, `frame_loop.h` | Scene publication tied to completed viewport frames |
| `src/blink/remote_frame.cc` | BSP composition, targeted scroll IPC, acknowledgement and scene lifecycle |
| `src/blink/browser_frame.cc`, `browser_frame.h` | Apply absolute offsets to the identified Blink scroll area |
| `src/process/renderer_runtime.c`, `src/posix.c` | Shared entropy bounce buffer and private-mapping regression probe |
| `config/blink-navigation.gn` | Compile the compositor implementation |
| `tests/input-scenario.py`, `tests/input-pages/scroll-tree.html`, `tests/input-pages/entropy.html` | QMP scenarios and controlled fixtures |
| `README.md`, `NOTICE.md`, `validation/SCROLL-RECOVERY.md` | Current architecture, limitations and historical validation context |

## Remaining boundaries

- Wheel listeners use the ordinary event path, including passive listeners. Cancelable wheel behavior is not bypassed.
- Non-translation transforms, path clips, filters, horizontal offsets, scroll snapping and repaint-dependent scroll backgrounds fall back to the renderer.
- Keyboard scrolling, scrollbar dragging and scrollbar-thumb updates still depend on renderer progress. Scroll-linked JavaScript and newly loaded or mutated content cannot update during an uninterrupted JavaScript task.
- A tile is drawn synchronously. An expensive individual Skia operation can delay the BSP; no worst-case input latency or physical display latency is guaranteed.
- The single-CPU fallback has no independent compositor execution while JavaScript runs. Physical hardware and broad website compatibility remain unvalidated.
