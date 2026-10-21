# Asynchronous scrollbar appearance

This report describes its recorded image. See [gutter reflow validation](SCROLL-GUTTER.md) for the subsequent outer-thickness and document-layout implementation.

This report preserves the earlier image and results. [Scrollbar layout validation](SCROLL-LAYOUT.md) supersedes the fixed internal part-geometry limitation; outer gutter and document reflow remain renderer work.

Validation date: 2026-10-19. This extends [two-axis scrollbar controls](SCROLL-CONTROLS.md) without adding a dependency. The tested EFI image SHA-256 is `37e912e1d9a42a39567dd06024d1a90e0749c31d2ee9d550da49fbf2a6404acf`.

## Implementation

Scrollbar pictures carry eight combinations of hover, pressed and disabled state. Native parts use a detached paint proxy. CSS parts use a detached `CustomScrollbar` and an isolated layout part, with a UEFI-only style-resolution helper in `patches/blink-build.patch`. Capture does not change the live scrollbar's input state. Identical variants share serialized and decoded data and count once toward the scene byte budget. Tile cache keys include appearance.

The BSP hit-tests the current scene and selects the appropriate picture before queued renderer input runs. Thumb capture retains pressed state when the pointer leaves the thumb. Release and toolbar departure update the visible state immediately. A synthetic pointer departure also clears Blink's hover state when it resumes. New renderer publications retain local interaction state when their painted state differs. Hiding a tab clears capture and hover.

Native arrows become disabled at the relevant scroll boundary. CSS `:disabled` follows Blink's whole-scrollbar enabled state rather than treating every endpoint arrow as a disabled CSS scrollbar. Disabled controls consume presses without changing offsets or starting repeat. Zero-range controls use a stationary scene transform because they may not have a Blink scroll translation.

The first forwarded mouse input establishes fine-pointer and hover capabilities in Blink settings. This enables native hover painting and makes pointer capability media queries match the available input path.

Source changes are in `src/blink/scroll_snapshot.h`, `scroll_scene.h`, `scroll_scene.cc`, `remote_frame.h`, `remote_frame.cc`, `browser_frame.h`, `browser_frame.cc`, `browser_entry.cc` and `patches/blink-build.patch`. The fixture and QEMU assertions are in `tests/input-pages/scroll-style.html`, `tests/scroll-style.py` and `tests/input-scenario.py`.

## Controlled QEMU results

The fixture blocks the renderer with uninterrupted JavaScript for 25 seconds. The harness sends real QMP mouse and keyboard events and checks framebuffer pixels before the JavaScript completion marker. It does not inject DOM state through a debugger. The results (`scroll-style-results.json`; local artifact) and serial log (`scroll-style.log`; local artifact) record:

| Check | Result |
| --- | --- |
| Custom thumb states on both axes | Normal, hover, pressed with hover, pressed outside and release passed |
| Custom arrow and track states | Hover, pressed and release/departure passed |
| Exact custom-state color checks | 18 checks passed |
| Native thumb | Hover and pressed pixels changed in a crop excluding the cursor |
| Native end arrow | Disabled pixels changed after dragging to the maximum offset |
| Disabled CSS control | Remained disabled during pointer movement and press; no asynchronous offset command |
| Toolbar departure | Page hover cleared before JavaScript completed |
| Pointer capabilities | `hover: hover` and `pointer: fine` matched |
| Renderer reconciliation | No RGB channel differed by more than 2 in rectangle `(60,230)` to `(1200,770)` |
| Shutdown | EFI loader released payload, stack and TLS |

Evidence: pressed custom thumb (`scroll-style-thumb-pressed-hover.png`; local artifact), pressed outside (`scroll-style-thumb-pressed-outside.png`; local artifact), disabled control (`scroll-style-disabled-pressed.png`; local artifact), native end arrow (`scroll-style-native-end.png`; local artifact), toolbar departure (`scroll-style-toolbar-leave.png`; local artifact), last frame during JavaScript (`scroll-style-final-busy.png`; local artifact) and reconciled frame (`scroll-style-reconciled.png`; local artifact).

Two runtime defects were reproduced and corrected during validation. A disabled scrollbar without a scroll translation initially rejected asynchronous scene capture; the stationary transform fixes that case. Native hover initially produced identical pixels because Blink's available hover capabilities were unset; initializing capabilities from actual mouse input fixes that case. The final dedicated scenario passed both checks.

## Regression coverage

The same-image suite (`scroll-style-suite.json`; local artifact) passed all seven scenarios: `scroll-controls`, `scroll-input`, `scroll-site-input`, `scroll-tree`, `wheel-fixture`, `tabs-site` and `renderer-recovery`. These cover both-axis custom/native controls and RTL offsets, keyboard and listener exclusions, nested/sticky scrolling, cancelable wheel events, live-site tab switching and renderer termination/exception recovery. Each scenario completed EFI teardown.

The live Python documentation results (`scroll-style-scroll-site-input-scroll-site-results.json`; local artifact) record independent sidebar and document scroll targets. Its framebuffer (`scroll-style-scroll-site-input-input-site-input-document-after.png`; local artifact) and the restored zeroday0619.dev tab (`scroll-style-tabs-site-input-tabs-site-restored.png`; local artifact) were visually inspected. The recovery log (`scroll-style-renderer-recovery.log`; local artifact) records the renderer fault and recovery checks. These site regressions complement the controlled long-task style test; they do not measure style latency across arbitrary websites.

## Build and checks

Blink compilation and EFI linking passed. Binary validation (`scroll-style-binary.log`; local artifact) found no required unresolved symbols or OS syscall instructions. Pump tests (`scroll-style-pump.log`; local artifact), EEVDF/network coroutine tests and 4,128 SIMD cases (`scroll-style-scheduler.log`; local artifact) passed. Static checks and source hashes (`scroll-style-static-checks.json`; local artifact) record Python compilation, fixture JavaScript syntax and `git diff --check`. The persisted custom-scrollbar patch passed `git apply --reverse --check` against the compiled Chromium checkout.

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 tests/redirect-server.py
# Run each QEMU scenario sequentially while the fixture server is active.
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=scroll-style python3 tests/browser-qemu.py
for scenario in scroll-controls scroll-input scroll-site-input scroll-tree wheel-fixture tabs-site; do
  QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO="$scenario" python3 tests/browser-qemu.py || exit 1
done
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 QEMU_TEST_GDB_PORT=12345 INPUT_SCENARIO=renderer-recovery python3 tests/browser-qemu.py
bash tests/pump-loop.sh
bash tests/network-scheduler.sh
python3 -m py_compile tests/scroll-style.py tests/input-scenario.py
git diff --check
```

Environment: Chromium 158.0.8110.0, Clang 23.1.3, musl 1.2.5, QEMU 11.1.2 with KVM, OVMF 2026.08+ds-2, four vCPUs, 4 GiB RAM and a 1280-by-800 framebuffer with a 1280-by-720 page viewport.

## Remaining limits

State variants retain geometry from the captured scene. Hover or pressed styles that change control dimensions or page layout need a new renderer publication. New CSS, DOM, resources and content also require renderer progress. Full-track clipping does not reproduce every gradient, image or border tied to a changing track-piece size. Arbitrary HTML/JavaScript scrollbar widgets still need their handlers to run.

The scene and tile budgets remain bounded. Unsupported transforms, clips, effects and input handlers retain existing renderer fallback. Individual Skia operations still run synchronously on the BSP. These tests demonstrate state changes during blocked JavaScript; they do not establish worst-case or physical input-to-display latency. Physical hardware and all CSS scrollbar combinations were not tested. Human review is required before merging.
