# Two-axis asynchronous scrollbar controls

This report preserves the earlier image and its validation results. [Scrollbar appearance validation](SCROLL-STYLE.md) supersedes the hover, pressed and disabled styling limitation below.

Validation date: 2026-10-19. This extends [asynchronous input](SCROLL-INPUT.md) without adding a dependency. The firmware image SHA-256 is `cba7bbcafd0d342da77d1bf89fe8916e2c8d910c3bd3db93dfc7e7d9eb5cc190`.

## Implementation

`ScrollTransform` now records both offsets, minimum and maximum positions, viewport dimensions and per-axis scroll eligibility and overscroll propagation. Captured nonzero horizontal offsets no longer disable the scene. Sticky constraints receive both ancestor coordinates. Absolute renderer commands carry X and Y together, so a vertical operation preserves the current horizontal position and RTL containers can use negative horizontal coordinates.

Native scrollbar display items and CSS `::-webkit-scrollbar` parts are serialized as separate scene pictures. Custom controls use Blink's scrollbar hit-test chunk, layout parts, theme rectangles and paint order. A custom scrollbar whose paint properties differ across its parts disables asynchronous capture for that scene. The BSP reconstructs its own Skia resources; it does not dereference renderer layout objects. Thumb translations depend on the axis, range and constrained track extent. Back and forward track pieces are recorded across the full track and clipped at the moving thumb midpoint.

The BSP handles thumb capture, all four custom arrow positions, track paging and repeat timing. Arrow steps are 40 CSS pixels; page steps are seven eighths of the relevant viewport. Repeat uses Blink's initial delay and interval, with a minimum interval of 16 ms. It pauses outside the relevant button or track, stops paging when the thumb reaches the pointer, and ends on release, tab hiding, navigation or recovery. A missed deadline generates one repeat instead of a burst. Renderer scene replacement retains a valid captured scrollbar even when its image/font resources change. Eligible ArrowLeft and ArrowRight use the acknowledged focus target and preserve the existing keyboard-handler and form-control exclusions.

Changed source files are `src/blink/scroll_snapshot.h`, `scroll_scene.h`, `scroll_scene.cc`, `remote_frame.cc`, `browser_frame.h` and `browser_frame.cc`. The test adds `tests/input-pages/scroll-controls.html`, `tests/scroll-controls.py` and the `scroll-controls` branch in `tests/input-scenario.py`.

## Controlled QEMU results

The [fixture](../tests/input-pages/scroll-controls.html) has custom and native nested containers, a horizontally scrollable document, sticky elements and an RTL mode. Three consecutive modes each occupy the renderer with uninterrupted JavaScript for 20 seconds. Input enters through QEMU's keyboard and virtio mouse devices. The harness checks serial commands and captured pixels before each long task ends, then compares the final renderer image and DOM scroll offsets. It does not inject DOM commands through a debugger.

The [results](scroll-controls-results.json) and [serial log](scroll-controls.log) record:

| Check | Observed result |
| --- | --- |
| Horizontal ArrowRight to changed framebuffer capture | 61.73 ms, one sample including QMP, polling and host capture overhead |
| Custom controls during JavaScript | Both-axis thumb dragging; eight arrow clicks covering both ends and directions; four track clicks; Left/Right keys |
| Held arrow | 12 repeat commands; moving outside paused repetition; release stopped it |
| Held native track | Repetition stopped after the thumb reached the pointer |
| Native controls during JavaScript | Horizontal and vertical arrow and track input; root horizontal thumb dragging |
| Reconciled custom offsets | X = 1,813 px and Y = 3,179 px |
| Reconciled native and root offsets | Nested X = 1,312 px, Y = 376 px; root X = 590 px |
| RTL input | Left produced -40 px, Right restored 0 px, thumb dragging reached -1,325 px |
| LTR reconciliation crop | 0.002167% of RGB channels differed by more than 2 |
| Native reconciliation crop | 0.025000% of RGB channels differed by more than 2 in `(700,250)` to `(1200,650)` |
| RTL reconciliation crop | 0.001667% of RGB channels differed by more than 2 |
| Shutdown | EFI loader released payload, stack and TLS |

The LTR and RTL image comparisons use framebuffer rectangle `(60,250)` to `(560,650)`, including custom content, the sticky region and both scrollbar tracks. Screenshots were visually inspected. Evidence: [initial frame](scroll-controls-initial.png), [horizontal drag during JavaScript](scroll-controls-horizontal-thumb-busy.png), [vertical drag during JavaScript](scroll-controls-vertical-thumb-busy.png), [LTR reconciliation](scroll-controls-reconciled.png), [native reconciliation](scroll-controls-native-reconciled.png), [RTL during JavaScript](scroll-controls-rtl-busy.png) and [RTL reconciliation](scroll-controls-rtl-reconciled.png).

Other per-action durations in the JSON include deliberate pointer movement and key/button sleeps. They are test-operation durations, not input latency measurements. Neither the single key sample nor the reconciliation comparison establishes a worst-case or physical input-to-display latency bound.

## Regression coverage

The [same-image regression suite](scroll-controls-suite.json) passed `scroll-input`, `scroll-site-input`, `scroll-tree`, `wheel-fixture`, `tabs-site`, `renderer-recovery` and the final `scroll-controls` rerun. This covers cancelable listeners, text/select defaults, nested/sticky scrolling, a live Python documentation page, live-site tab switching and renderer termination/exception recovery. The live-site scrollbar [result](scroll-controls-scroll-site-input-scroll-site-results.json) and [framebuffer](scroll-controls-scroll-site-input-input-site-input-document-after.png) show independent sidebar and document scrolling. Recovery [serial evidence](scroll-controls-renderer-recovery.log) records tracked allocation reclamation after AP quiescence. Every scenario completed EFI teardown.

## Build and checks

Blink compilation and the final EFI link passed. [Binary validation](scroll-controls-binary.log) found no required unresolved symbols, PE imports or OS syscall instructions. [Pump tests](scroll-controls-pump.log), [EEVDF/network coroutine tests and 4,128 SIMD cases](scroll-controls-scheduler.log), Python compilation, fixture JavaScript syntax and `git diff --check` passed.

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 tests/redirect-server.py
# Run QEMU scenarios sequentially while the fixture server is active.
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=scroll-controls python3 tests/browser-qemu.py
bash tests/pump-loop.sh
bash tests/network-scheduler.sh
python3 -m py_compile tests/scroll-controls.py tests/input-scenario.py
```

Environment: Chromium 158.0.8110.0, Clang 23.1.3, musl 1.2.5, QEMU 11.1.2 with KVM, OVMF 2026.08+ds-2, four vCPUs, 4 GiB RAM and a 1280-by-800 framebuffer with a 1280-by-720 page viewport.

## Remaining limits

CSS scrollbar geometry responds during the long task, but hover, pressed and disabled styling refreshes with renderer publications. Full-track clipping does not reproduce every gradient, image or border whose appearance depends on the changing size of an individual track piece. Arbitrary HTML/JavaScript scrollbar widgets still require their event handlers to run. Horizontal hardware-wheel reports, touchpad gestures and kinetic scrolling are not implemented; horizontal input uses the scrollbar or eligible Left/Right keys.

Cancelable DOM handlers, editing, scroll snapping, non-translation transforms, path clips, filters and repaint-dependent backgrounds retain renderer fallback. New layout or style changes need a new scene. Individual Skia operations remain synchronous on the BSP. Physical hardware, all CSS scrollbar styles and worst-case latency have not been validated. Human review is required before merging the changes.
