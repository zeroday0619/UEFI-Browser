# Asynchronous scrollbar gutter reflow

This report records the earlier single-document implementation and its EFI image. Container-query and local iframe support is described in [the frame-tree validation report](GUTTER-TREE.md). The exclusions below apply to the image tested here.

## Implementation

Custom scrollbar hover and pressed states can change the width or height available to the document. The renderer now captures complete Blink layout/paint scenes for the supported thickness combinations. The BSP selects the corresponding scene while the renderer is executing JavaScript. Text, Grid/Flex geometry, clips, wheel regions, scroll ranges and sticky constraints change together. This uses precomputed layouts of the last published DOM; it does not execute Blink concurrently on the BSP.

`CustomScrollbar` exposes detached thickness measurement, temporary thickness overrides and compositor-state application. UEFI hover/pressed updates invalidate the owner layout when the outer thickness changes. `LocalFrameView` provides a script-forbidden snapshot path through style/layout, compositing inputs and prepaint. Capture bypasses animation, observer, focus and normal post-layout notification steps. Intermediate animation-frame events and scroll anchor capture are suppressed. GC roots retain scrollbars and scroll areas across speculative layout. Restoring a replaced scrollbar also restores its input state. The original geometry and offsets are restored before returning to the event loop.

The frame loop ignores visual-update requests caused by speculative capture. The compositor decodes all candidate scenes before acknowledging the renderer publication. It retains BSP-owned resources, shares matching fonts/images across layouts and discards the displaced scene's tiles. Scene switches preserve absolute scroll offsets and drag capture. Gutter commands precede subsequent pointer/scroll commands and use the existing sequence acknowledgement. The renderer lays out the accepted gutter state before queued content hit tests and cancels deferred anchoring for that switch.

A QEMU failure exposed a pre-existing drag quantization problem: moving only across the scrollbar converted its rounded thumb position back into a different scroll offset. Dragging now changes the offset only when the pointer moves along the scrolling axis. Track-jump initialization still performs its initial absolute move.

## Limits

- Up to 16 distinct thickness combinations and 16 MiB of serialized resources and geometry are cached. Individual thicknesses must be in the range 1–256 pixels.
- Native `scrollbar-width` changes and host-element hover rules require renderer progress. The cache resolves custom `::-webkit-scrollbar` pseudo-element hover/pressed states.
- Container-query layouts, embedded frames, scrollbar creation/removal, changing thickness rules across candidate layouts, unsupported paint properties and exhausted budgets retain renderer dispatch.
- DOM reads, ResizeObserver callbacks and script handlers reflect the accepted state after the renderer resumes. Cached scenes represent the last published DOM.
- Gutter switches preserve absolute scroll offsets. They do not apply asynchronous scroll-anchor adjustments.
- Existing custom track-piece gradient/border limitations remain. Cached reflow does not implement arbitrary concurrent DOM mutation or unrestricted CSS execution on the BSP.

## Validation

Validated on 2026-10-20 with Chromium 158.0.8110.0 at `f12c350cb1e1e7ea05c11d6045b0a04158ec9e28`, clang 23.1.3, musl 1.2.5, QEMU 11.1.2 and OVMF 2026.08. QEMU used KVM, four virtual CPUs, 4 GiB RAM and a 1280×800 framebuffer with a 1280×720 page viewport.

The final EFI SHA-256 is `8fbce47fcf747c473cfd2b23cec0f2605f0a040c576675fd5e5772e9ccdf2af2`. Static checks (`scroll-gutter-static-checks.json`; local artifact) record the source hashes and confirm that the Chromium adapter mirror matches the project sources. Suite results (`scroll-gutter-suite.json`; local artifact) identify the same image and the tested fixture sources.

### Reproduction and measured behavior

The previous EFI failed the nested vertical fixture before implementation: the hover state retained a 476-pixel text column instead of reducing it to 420 pixels. The marker remained at framebuffer Y=368. See the baseline result (`scroll-gutter-before.json`; local artifact), runner output (`scroll-gutter-before-runner.log`; local artifact) and hover screenshot (`scroll-gutter-before-hover-reference.png`; local artifact). The baseline records its separate EFI and test hashes.

The final fixtures execute an uninterrupted 25-second JavaScript loop. QMP sends pointer movement, press, release and cross-axis movement while the loop is running. Framebuffer pixels establish the text-column width and the position of a marker after the text. Each assertion also checks that JavaScript has not finished. Serial publication counts establish that the renderer did not publish a frame during these input transitions.

Values below follow normal → hover → pressed order. Marker Y includes the 80-pixel browser toolbar.

| Fixture | Scrollbar thickness (px) | Text-column width (px) | Text-end marker Y (px) | Result |
| --- | --- | --- | --- | --- |
| Nested vertical | 24 → 80 → 120 | 476 → 420 → 380 | 368 → 400 → 400 | Passed |
| Root vertical | 24 → 80 → 120 | 1256 → 1200 → 1160 | 368 → 400 → 400 | Passed |
| Horizontal | 16 → 48 → 80 | 262 → 246 → 230 | 440 → 440 → 464 | Passed |

The horizontal fixture uses an aspect-ratio column whose width depends on the available height. Increasing scrollbar height therefore changes text wrapping as well as the viewport height. The vertical fixtures begin at scroll offset 80 with scroll anchoring enabled. Nested document scroll height changes from 1932 to 2252 pixels; root document scroll height changes from 2212 to 2372 pixels when hovering. All three fixtures restore normal geometry on release and retain pressed geometry while the pointer leaves the scrollbar across its axis.

Each fixture recorded zero renderer publications during the busy input sequence and zero publications during a one-second idle check. No additional ResizeObserver callback was delivered during that idle interval. After JavaScript completed, Blink reported the accepted client dimension. Comparing the affected viewport crop before and after renderer reconciliation produced a difference fraction of **0.0** in all three cases. The comparison counts color components differing by more than two levels; its acceptance threshold is 2%. These measurements do not establish optical input latency or a frame-rate guarantee.

- Nested: measurements (`scroll-gutter-scroll-gutter-scroll-gutter-results.json`; local artifact), busy pressed screenshot (`scroll-gutter-scroll-gutter-input-gutter-pressed-busy.png`; local artifact), serial log (`scroll-gutter-scroll-gutter.log`; local artifact).
- Root: measurements (`scroll-gutter-scroll-gutter-root-scroll-gutter-results.json`; local artifact), busy pressed screenshot (`scroll-gutter-scroll-gutter-root-input-gutter-pressed-busy.png`; local artifact), serial log (`scroll-gutter-scroll-gutter-root.log`; local artifact).
- Horizontal: measurements (`scroll-gutter-scroll-gutter-horizontal-scroll-gutter-results.json`; local artifact), busy pressed screenshot (`scroll-gutter-scroll-gutter-horizontal-input-gutter-pressed-busy.png`; local artifact), serial log (`scroll-gutter-scroll-gutter-horizontal.log`; local artifact).

The container-query fixture verifies fallback: its text-column width remains 476 pixels during the blocking script and becomes 420 pixels after the renderer resumes. It produces no cached gutter transitions. See fallback measurements (`scroll-gutter-scroll-gutter-fallback-scroll-gutter-fallback-results.json`; local artifact). The cross-axis drag regression is recorded in the failed intermediate run (`scroll-gutter-drag-before-runner.log`; local artifact) and serial log (`scroll-gutter-drag-before.log`; local artifact); the final three reflow fixtures cover the correction.

### QEMU regression results

All 13 scenarios passed sequentially against the final EFI. Durations include boot, fixture loading, deliberate JavaScript blocking and teardown; they are not performance measurements.

| Scenario | Duration (s) | Result |
| --- | ---: | --- |
| `scroll-gutter` | 63.72 | Passed |
| `scroll-gutter-root` | 66.99 | Passed |
| `scroll-gutter-horizontal` | 68.24 | Passed |
| `scroll-gutter-fallback` | 58.44 | Passed |
| `scroll-layout` | 55.86 | Passed |
| `scroll-controls` | 99.76 | Passed |
| `scroll-style` | 52.84 | Passed |
| `scroll-input` | 96.53 | Passed |
| `scroll-site-input` | 52.34 | Passed |
| `scroll-tree` | 48.88 | Passed |
| `wheel-fixture` | 27.95 | Passed |
| `tabs-site` | 41.34 | Passed |
| `renderer-recovery` | 69.46 | Passed |

Regression coverage includes scrollbar part dimensions, arrow and track clicks, horizontal and RTL scrolling, hover/pressed/disabled appearance, keyboard input, nested scroll and sticky elements, and wheel cancellation. The live Python documentation test covers independent sidebar/document scrolling and input. The live `zeroday0619.dev` test covers loading input, theme interaction, keyboard/wheel scrolling and tab switching. Renderer recovery covers termination of infinite JavaScript, the process-monitor button, page faults and invalid opcodes. Logs use the `scroll-gutter-<scenario>-runner.log` and `scroll-gutter-<scenario>.log` names in this directory.

### Build and static checks

The following commands passed from the project directory:

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
bash tests/pump-loop.sh
bash tests/network-scheduler.sh
git -C ../../work/chromium-build/chromium apply --reverse --check "$PWD/patches/blink-build.patch"
git diff --check
```

The Blink build log (`blink-native-library-build.log`; local artifact) records compilation. The binary validation (`scroll-gutter-binary.log`; local artifact) confirms a static x86-64 EFI image with bounded ELF loads, no required unresolved symbols and no syscall instructions. Pump checks (`scroll-gutter-pump.log`; local artifact) passed immediate, idle, pre-wait and nested quit behavior. Scheduler checks (`scroll-gutter-scheduler.log`; local artifact) passed eligibility, deadline selection, service fairness, cancellation and 4128 SSE2 raster cases. Python syntax compilation and JavaScript fixture syntax checks also passed. Full Chromium WPT and browser test suites were not run; runtime validation targets the UEFI image in QEMU.

### Reproduction commands

Run the fixture server in a separate terminal from the project directory:

```sh
python3 tests/redirect-server.py
```

Run each scenario sequentially against the built EFI:

```sh
for scenario in scroll-gutter scroll-gutter-root scroll-gutter-horizontal scroll-gutter-fallback \
    scroll-layout scroll-controls scroll-style scroll-input scroll-site-input scroll-tree \
    wheel-fixture tabs-site; do
    QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO="$scenario" \
        python3 tests/browser-qemu.py || break
done
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 QEMU_TEST_GDB_PORT=12345 \
    INPUT_SCENARIO=renderer-recovery python3 tests/browser-qemu.py
```

The fixture server is used for deterministic layout tests. The existing `scroll-site-input` and `tabs-site` scenarios also exercise live site behavior; their results apply to the content served during this run.

## Changed components

- [Blink patch](../patches/blink-build.patch): UEFI-only snapshot lifecycle, thickness overrides, event suppression and scroll-anchor handling.
- [Scroll scene](../src/blink/scroll_scene.cc), [scene declarations](../src/blink/scroll_scene.h) and [snapshot data](../src/blink/scroll_snapshot.h): bounded reflow capture, resource ownership, scene selection and drag-coordinate preservation.
- [Frame loop](../src/blink/frame_loop.cc): suppresses speculative repaint scheduling and publishes alternate scenes.
- [Remote frame](../src/blink/remote_frame.cc) and [browser frame](../src/blink/browser_frame.cc): ordered gutter commands and renderer reconciliation before queued hit testing.
- [Reflow test](../tests/scroll-gutter.py), [fallback test](../tests/scroll-gutter-fallback.py), [input dispatch](../tests/input-scenario.py) and four `tests/input-pages/scroll-gutter*.html` fixtures: QMP input, pixel assertions and renderer-publication checks.
