# Asynchronous wheel, keyboard and scrollbar input

This report records the earlier vertical-input image. The subsequent [two-axis scrollbar implementation and validation](SCROLL-CONTROLS.md) supersedes its custom-scrollbar, horizontal-offset and track/button limitations.

Validation date: 2026-10-19. This change extends the serialized scroll scene described in [SCROLL-TREE.md](SCROLL-TREE.md). It uses the existing AP renderer and BSP compositor without adding a dependency.

## Input dispatch

The scene no longer rejects a whole document because it has wheel listeners. It copies Blink's blocking-wheel rectangles with their transform and clip ancestry, including chunks without drawing items. A wheel outside those rectangles can update the compositor immediately. Passive listeners still receive the wheel event through the renderer, marked `kEventNonBlocking`. The renderer skips its ordinary default scroll for this notification so the queued absolute offset is applied once. A blocking region retains ordinary cancelable dispatch. If blocking handlers exist without recorded rectangles, or the 4,096-region budget is exceeded, wheel dispatch remains conservative across the scene.

The renderer publishes the eligible keyboard scroll target independently of frame repaint. The target must be the main document or a focused scroll container with no keyboard listeners on its event path. Editable elements, activation controls and focused subframes retain renderer dispatch. Focus-changing input receives a separate acknowledgement; queued clicks or keys cannot authorize scrolling from a stale focus response. Eligible ArrowUp/Down, PageUp/Down, Home/End, Space and Shift+Space update the same scroll tree and send absolute offsets to Blink. Modified shortcuts retain renderer dispatch. Keyboard listeners are not executed concurrently with an existing JavaScript task.

Native vertical scrollbar display items are replaced in the scene by separate serialized track and thumb pictures painted with Blink's theme. The compositor updates thumb translations from each scroll area's current offset, track length and thumb size. A thumb press captures its scroll element and pointer offset. Motion clamps the requested position to the scroll range; release ends capture. Hit testing respects picture order and clips so a painted overlay can block a thumb. Scene acknowledgements, tab navigation and renderer recovery retain the existing lifetime rules. Custom scrollbars and track/button clicks remain on Blink's ordinary input path.

## Controlled QEMU test

`tests/input-pages/scroll-input.html` supplies a long document, a fixed nested scroll container, sticky headers, a passive window wheel listener, a separate cancelable wheel region a text input and a select control. Five modes each run uninterrupted JavaScript for 12 seconds. `INPUT_SCENARIO=scroll-input` sends real firmware keyboard and mouse input through QMP and captures the framebuffer without DOM injection.

The test checks:

- Eight root keyboard commands and eight nested keyboard commands change the compositor offset before JavaScript returns.
- A passive wheel scroll changes pixels before its listener runs. Its eventual event is non-cancelable.
- A wheel in the blocking rectangle does not scroll early and its eventual `preventDefault()` succeeds.
- Root and nested thumb drags move both content and scrollbar pixels during the long task. Blink later applies the same final offsets without a second default scroll.
- A window keydown cancellation handler is honored. An editable input retains End and text insertion behavior, and a select control retains End selection, instead of scrolling the page.
- A framebuffer crop including the scrollbar regions is compared with the reconciled Blink frame. The captured arrow-button enabled appearance can differ until Blink publishes a new scene; exact full-frame equality is not asserted.

The initial fixture incorrectly allowed `body.focus()` to scroll the document by 100 pixels. The fixture now requests `preventScroll` and uses a non-collapsing spacer. That failed run is a fixture-coordinate error, not a successful behavioral validation.

## Delivered-image results

The delivered image SHA-256 is `dcbe54436d7beaccbc60ccb73a6f2ad057944a76d1c9d99434b9965559fcfe9c`. Its binary validation (`scroll-input-release-binary.log`; local artifact) passed with no required unresolved symbols or OS syscall instructions. The controlled result (`scroll-input-release-scroll-input-results.json`; local artifact) and serial log (`scroll-input-release-scroll-input.log`; local artifact) record the five long-task modes.

| Check | Result |
| --- | --- |
| First ArrowDown to changed framebuffer capture | 21.19 ms, one sample including QMP and host capture overhead |
| Keyboard scrolling while JavaScript runs | Eight root and eight nested commands passed |
| Native thumb dragging while JavaScript runs | Root reached 6,447 px; nested container reached 8,626 px |
| Passive wheel notification | Pixels changed during the long task; listener later received `cancelable=false` without a second default scroll |
| Cancelable wheel and keydown | Both cancellation handlers remained effective |
| Text and select controls | End followed by text input produced `unchangeda`; End selected option `C` |
| Reconciled frame | 0.009164% of RGB channels differed by more than 2 in rectangle `(400,80)` to `(1280,700)` |

The nontrivial reconciliation differences occurred in the two captured up-arrow glyphs, whose enabled appearance was refreshed by the renderer. Document and thumb positions matched. Screenshots: first key during JavaScript (`scroll-input-release-scroll-input-input-input-first-key-busy.png`; local artifact), root drag (`scroll-input-release-scroll-input-input-input-root-drag-busy.png`; local artifact), nested drag (`scroll-input-release-scroll-input-input-input-nested-drag-busy.png`; local artifact), nested keyboard input (`scroll-input-release-scroll-input-input-input-nested-key-busy.png`; local artifact) and reconciled frame (`scroll-input-release-scroll-input-input-input-reconciled.png`; local artifact).

## Live website and regression checks

The live-site case loads [Python's Event Loop documentation](https://docs.python.org/3/library/asyncio-eventloop.html), requires HTTP 200, and verifies distinct scroll IDs for the sidebar and document. It locates each native scrollbar thumb in the captured framebuffer and drags it through QMP. This case verifies an unmodified public website. The separate controlled page establishes behavior while JavaScript is blocked.

The six-scenario regression suite (`scroll-input-suite.json`; local artifact) passed the new input contracts, live website input, nested/sticky scrolling, cancelable wheel behavior, live-site tabs and renderer recovery. Its image hash is recorded separately in `scroll-input-image-sha256.txt` (local artifact). Recovery reclaimed every tracked broker allocation after each injected termination or exception.

The delivered image adds explicit form-control exclusion and preserves modified scan-key dispatch. On that image, the expanded five-mode input scenario, live-site scrollbar scenario and wheel regression were repeated successfully (`scroll-input-release-suite.json`; local artifact). The full six-scenario suite was not repeated on the delivered image. Combined results (`scroll-input-results.json`; local artifact) preserve this build boundary.

Live-site evidence: results (`scroll-input-release-scroll-site-results.json`; local artifact), serial log (`scroll-input-release-scroll-site-input.log`; local artifact), document after dragging (`scroll-input-release-scroll-site-input-input-site-input-document-after.png`; local artifact) and sidebar after dragging (`scroll-input-release-scroll-site-input-input-site-input-sidebar-after.png`; local artifact). The delivered-image wheel regression log (`scroll-input-release-wheel-fixture.log`; local artifact) also records successful EFI teardown. Pump tests (`scroll-input-pump.log`; local artifact), EEVDF/network coroutine tests and 4,128 SIMD cases (`scroll-input-scheduler.log`; local artifact), Python and fixture JavaScript syntax checks, and `git diff --check` passed. Source changes still require human review before merging.

Build and verification commands, run from the project directory:

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 tests/redirect-server.py
# Run these scenarios sequentially while the fixture server is active.
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=scroll-input python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=scroll-site-input python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=scroll-tree python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=wheel-fixture python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=tabs-site python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 QEMU_TEST_GDB_PORT=12345 INPUT_SCENARIO=renderer-recovery python3 tests/browser-qemu.py
bash tests/pump-loop.sh
bash tests/network-scheduler.sh
python3 -m py_compile tests/input-scenario.py tests/browser-qemu.py
# Extract the fixture's script to a temporary .js file and check it with node --check.
git diff --check
```

Environment: Chromium 158.0.8110.0, Clang 23.1.3, QEMU 11.1.2 with KVM, OVMF 2026.08+ds-2, four vCPUs, 4 GiB RAM and a 1280-by-800 framebuffer with a 1280-by-720 page viewport. QMP/capture timing is not physical input-to-display latency.

## Changed files

| Files | Change |
| --- | --- |
| `src/blink/scroll_scene.cc`, `scroll_scene.h`, `scroll_snapshot.h` | Blocking regions, keyboard eligibility, scrollbar capture, thumb transforms and drag hit testing |
| `src/blink/remote_frame.cc` | Keyboard target publication, focus acknowledgements and asynchronous input routing |
| `src/blink/browser_frame.cc`, `browser_frame.h` | Keyboard eligibility query and wheel notification without a second default scroll |
| `src/blink/input.h`, `src/process/renderer_protocol.h` | Value-only consumed-wheel flag carried through the existing input queue |
| `tests/input-pages/scroll-input.html`, `tests/input-scenario.py` | Long-task contracts, editable/cancelable regressions and live scrollbar input |
| `README.md`, `validation/SCROLL-TREE.md` | Updated support boundaries and validation links |

## Remaining boundaries

Cancelable handlers that cover the input target must finish before their default action can be known. Long JavaScript tasks therefore still delay scrolling in those regions. Passive callbacks, editing, keyboard handlers and scroll-linked scripts also wait for JavaScript execution to become available; only eligible visual scrolling proceeds independently.

Eligibility and geometry reflect the last renderer publication. New DOM, layout, listener and focus changes require renderer progress and publication. This implementation does not interrupt JavaScript to discover those changes. Native vertical thumb dragging is supported; custom scrollbars, track auto-repeat, horizontal scrolling, scroll snapping and complex transform/filter scenes retain the documented fallback paths. Individual Skia operations still run synchronously on the BSP. Physical hardware and worst-case latency are not validated.
