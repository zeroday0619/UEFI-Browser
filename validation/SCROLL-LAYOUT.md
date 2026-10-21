# Asynchronous scrollbar part layout

This report describes its recorded image. See [gutter reflow validation](SCROLL-GUTTER.md) for the subsequent outer-thickness and document-layout implementation.

Validation date: 2026-10-19. This extends [scrollbar appearance](SCROLL-STYLE.md) to internal part geometry. The EFI image SHA-256 is `e7b9751ac5df774ec213de233a17aa5af5de85f3a17ee61516b56ba5619ac31b`.

## Scope and implementation

The previous compositor selected state colors but retained the original button, track and thumb rectangles. Hover or pressed rules changing lengths, margins or part visibility therefore painted and hit-tested stale geometry while JavaScript blocked the renderer.

CSS scrollbar capture now enumerates hovered/pressed part pairs on a detached Blink `CustomScrollbar`. Ten targets, including no part, produce at most 100 profiles per enabled scrollbar. Disabled controls need one profile. The UEFI-only `PrepareForSnapshot` helper copies scroll proportions and the outer frame, resolves styles for every part and runs Blink's part-positioning code. Blink's existing CSS length, min/max, margin and visibility calculations supply geometry; the BSP does not implement a separate CSS parser or run page script.

Each profile contains track, thumb and four button rectangles plus correctly sized part paint records. Empty parts remain represented so `display:none` transitions can remove or restore them. When combined button lengths exceed the frame length, Blink's `HasButtons` rule hides them and releases the track space. The BSP selects the profile and updates picture bounds, hit testing, thumb travel, clipping and repeat targets together. Captured thumb offsets compensate for changes in geometry before applying pointer motion. Scene replacement preserves the local hovered part and applies its geometry before hit testing, so a pointer inside an expanded button does not switch to its neighbor when a new frame arrives. Native controls retain the existing eight appearance variants. Styles are memoized by the two varying selector-state bits for each part within one capture. Equal styles and rectangles reuse paint results before recording and serialization. Equal paint records share serialized and decoded data, and the existing scene and tile budgets still apply.

Detached part invalidation updates only the proxy. It does not notify the live scrollable area or schedule another frame. The initial implementation omitted this boundary and entered a capture/repaint loop on the gradient-heavy controls fixture. GDB evidence (`scroll-layout-before-fix-heap.log`; local artifact) recorded the 128 MiB heap exhausted with seven 16,782,336-byte raster buffers retained. Style/paint memoization alone did not fix that feedback loop; suppressing owner invalidation addresses its trigger. The layout test checks for repeated scene publications while idle.

Scene installation initializes the new transform offsets before clearing or restoring capture. An early QEMU run exposed an out-of-bounds access when cancellation applied new scrollbar geometry using the previous scene's transform offsets; the installation order was corrected before the successful runs.

Changed implementation files are `src/blink/scroll_snapshot.h`, `scroll_scene.h`, `scroll_scene.cc` and `patches/blink-build.patch`. The patch updates Chromium's `core/layout/custom_scrollbar.h` and `.cc`. Tests add `tests/input-pages/scroll-layout.html`, `tests/scroll-layout.py` and the `scroll-layout` dispatch in `tests/input-scenario.py`.

## Controlled QEMU results

The [fixture](../tests/input-pages/scroll-layout.html) runs uninterrupted JavaScript for 25 seconds. The [test](../tests/scroll-layout.py) sends QMP keyboard and virtio mouse input, checks exact pixel colors and measured dimensions while the completion marker is absent, then compares the renderer's resumed output. No debugger injects DOM state.

The results (`scroll-layout-results.json`; local artifact) and serial log (`scroll-layout.log`; local artifact) record:

| Check | Observed result |
| --- | --- |
| Vertical thumb | Normal 40 px, hover 80 px, hover plus pressed 140 px, pressed outside 120 px, release 40 px |
| Track margins | Thumb top moved from framebuffer Y = 278 to 290 on hover and 302 on press |
| Buttons | Hover 42 px, press 60 px, release 42 px; `calc()` and max-size constraints were applied |
| Expanded button hit test | The newly occupied portion remained the backward button; pressing it at the minimum offset emitted no forward scroll command |
| Hidden end button | `display:none` removed the pressed button; the remaining end button occupied the released space |
| Oversized buttons | 600 px pressed button triggered Blink's button-hiding rule; the track expanded to the full frame |
| Idle scene publications | Zero during the one-second observation window |
| Horizontal thumb | Hover 100 px, press 160 px, release 100 px |
| Drag during enlarged state | 80 px of pointer movement produced 79 px of thumb movement, within the one-pixel integer rounding tolerance |
| Geometry measurements | 16 exact-length checks passed before JavaScript completed |
| Renderer reconciliation | No RGB channel differed by more than 2 in framebuffer rectangle `(60,230)` to `(560,610)` |
| Renderer offsets | Both nonzero scroll coordinates reached Blink after the long task |
| Shutdown | EFI loader released payload, stack and TLS |

Evidence: initial controls (`scroll-layout-initial.png`; local artifact), hovered thumb (`scroll-layout-vertical-hover.png`; local artifact), pressed thumb (`scroll-layout-vertical-pressed.png`; local artifact), expanded button (`scroll-layout-button-pressed.png`; local artifact), hidden end button (`scroll-layout-end-button-hidden.png`; local artifact), oversized button fallback (`scroll-layout-oversized-button-track.png`; local artifact), horizontal drag (`scroll-layout-horizontal-drag.png`; local artifact), last busy frame (`scroll-layout-final-busy.png`; local artifact) and reconciled frame (`scroll-layout-reconciled.png`; local artifact). The pressed-thumb framebuffer was visually inspected.

## Regression coverage

The same-image suite (`scroll-layout-suite.json`; local artifact) passed all nine scenarios: `scroll-layout`, `scroll-controls`, `scroll-style`, `scroll-input`, `scroll-site-input`, `scroll-tree`, `wheel-fixture`, `tabs-site` and `renderer-recovery`. Every scenario reached the EFI loader's payload, stack and TLS release marker. The existing appearance test retained all 18 color checks, native hover/pressed states, disabled-input handling and toolbar departure behavior.

The live Python documentation results (`scroll-layout-scroll-site-input-scroll-site-results.json`; local artifact) and framebuffer (`scroll-layout-scroll-site-input-input-site-input-document-after.png`; local artifact) show independent sidebar and document scrolling; the framebuffer was visually inspected. The site tab test (`scroll-layout-tabs-site.log`; local artifact) exercised zeroday0619.dev and settings tabs. The recovery log (`scroll-layout-renderer-recovery.log`; local artifact) covers forced renderer termination, an injected exception and navigation after recovery. These are compatibility regressions; the controlled fixture provides the blocked-JavaScript geometry evidence.

## Build and reproduction

Blink compilation and EFI linking passed. Binary validation (`scroll-layout-binary.log`; local artifact) found no required unresolved symbols or OS syscall instructions. Message-pump tests (`scroll-layout-pump.log`; local artifact) and EEVDF/network coroutine tests plus 4,128 SIMD cases (`scroll-layout-scheduler.log`; local artifact) passed. Static checks (`scroll-layout-static-checks.json`; local artifact) also passed Python compilation, fixture JavaScript syntax, patch consistency against the compiled Chromium checkout and `git diff --check`.

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 tests/redirect-server.py
# Run scenarios sequentially while the fixture server is active.
for scenario in scroll-layout scroll-style scroll-controls scroll-input scroll-site-input scroll-tree wheel-fixture tabs-site; do
  QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO="$scenario" python3 tests/browser-qemu.py || exit 1
done
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 QEMU_TEST_GDB_PORT=12345 INPUT_SCENARIO=renderer-recovery python3 tests/browser-qemu.py
bash tests/pump-loop.sh
bash tests/network-scheduler.sh
python3 -m py_compile tests/scroll-layout.py tests/input-scenario.py
git diff --check
```

Environment: Chromium 158.0.8110.0, Clang 23.1.3, musl 1.2.5, QEMU 11.1.2 with KVM, OVMF 2026.08+ds-2, four vCPUs, 4 GiB RAM and a 1280-by-800 framebuffer with a 1280-by-720 page viewport.

## Remaining limits

The image recorded in this report implemented scrollbar part layout within the captured outer frame. At that point, outer gutter changes and surrounding text wrapping required renderer progress. The subsequent [gutter reflow implementation](SCROLL-GUTTER.md) adds bounded precomputed document layouts and updates the UEFI hover/pressed invalidation path. Arbitrary concurrent document layout on the BSP remains unsupported.

Track pieces still use full-track pictures clipped at the moving thumb midpoint. Size-dependent gradients, borders and images on those pieces are not fully reproduced. New DOM, styles and resources, arbitrary JavaScript scrollbar widgets and existing unsupported compositor effects retain renderer fallback. Individual Skia operations remain synchronous on the BSP. These checks establish behavior during blocked JavaScript, not a worst-case latency bound. Physical hardware and every CSS state combination were not tested. Human review is required before merging.
