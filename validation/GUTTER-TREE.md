# Container-query and iframe gutter reflow

## Implementation

The compositor can select cached Blink reflows that include size-container-query results and local iframe content while the renderer executes an uninterrupted script. The renderer captures the local frame tree as one scene. Scroll-area lookup, custom scrollbar identification, paint properties, clips, wheel-handler regions and acknowledged scroll commands now cover descendant frames. Input coordinates remain in the root widget coordinate system; Blink's existing property tree supplies iframe translations and clipping.

The speculative layout path marks every local frame as being captured before running recursive style/layout and prepaint. Intermediate script, observer and scroll-event delivery remain suppressed. Each frame's scroll offsets are restored after capture. Local child viewport geometry is propagated before recursively laying out its document; plugin geometry callbacks remain excluded. When a container query replaces a layout object, restoration finds its scroll area through the retained DOM owner. A changed scrollbar topology rejects the alternate cache and recaptures the restored base scene so that asynchronous input cannot use obsolete scroll IDs.

The embedder previously returned no child frame from `CreateChildFrame`. It now creates local child frames with their own navigation request, interface broker and policy receiver. Children share the parent renderer task sequence and non-composited widget. HTTP(S) GET navigation reuses the asynchronous network loader with Blink's original navigation parameters, including initiator origin and frame policy. `about:blank` and `srcdoc` inherit parent policies; sandbox flags are combined with the embedding frame's flags. Frame detach cancels pending work through weak callbacks and releases the navigation request.

## Scope and limits

- Cached combinations remain limited to 16, with a 16 MiB serialized-data budget and thicknesses from 1 to 256 pixels. Local frame trees are limited to 16 frames including the main frame.
- Supported scenes include ordinary text reflow, Grid layout changes from size container queries, child-frame scrollbars and child reflow caused by a parent scrollbar changing the iframe width.
- Scrollbar creation, removal or replacement, changing thickness rules, unsupported paint properties and exhausted budgets use renderer dispatch. A query that changes only ordinary content layout does not trigger this fallback.
- Scroll-state and anchored container queries require lifecycle snapshots outside the speculative size-layout pass and retain renderer dispatch.
- Remote/out-of-process iframes are unsupported. Cross-origin local frames retain Blink origin checks but do not gain separate process isolation.
- Child navigation supports GET, `about:blank` and `srcdoc`. This work does not add POST navigation, child popup support or complete child-frame history management.
- Cached scenes represent the last published DOM. Live DOM dimensions, observers and script handlers catch up after the renderer resumes. This is not concurrent execution of Blink layout on the BSP.
- Existing native `scrollbar-width`, host-hover rules, asynchronous scroll anchoring and custom track-piece paint limitations remain.

## Validation

Validation uses Chromium 158.0.8110.0 at `f12c350cb1e1e7ea05c11d6045b0a04158ec9e28`, clang 23.1.3, musl 1.2.5, QEMU 11.1.2 and OVMF 2026.08. The run date is 2026-10-20. QEMU uses KVM, four virtual CPUs, 4 GiB RAM and a 1280×800 framebuffer.

The final EFI SHA-256 is `11c9cdc7cd4e3a0e6f3ecece7df7875ef3eea3ffc349250327079aefb7826a82`. [Static checks](gutter-tree-static-checks.json) record exact source hashes and the matching Chromium adapter mirror. [Binary validation](gutter-tree-binary.log) confirms a static x86-64 EFI image with bounded ELF loads, no required unresolved symbols and no syscall instructions. [Suite results](gutter-tree-suite.json) record completed QEMU cases against that image.

### Baseline

The earlier EFI (`8fbce47fcf747c473cfd2b23cec0f2605f0a040c576675fd5e5772e9ccdf2af2`) failed the new container-query test during the blocking script: hover retained a 476-pixel text column instead of 420 pixels. Its iframe test never reached the child document's ready marker because child frame creation was unimplemented. See the [container runner](gutter-tree-before-container-runner.log), [container serial log](gutter-tree-before-container.log), [iframe runner](gutter-tree-before-iframe-runner.log) and [iframe serial log](gutter-tree-before-iframe.log).

### Parent-to-child geometry correction

The first frame-tree build updated the iframe clip but left the child layout viewport at its old width during speculative capture. Hover produced a 1200-pixel visible column while its text-end marker stayed at Y=368; the live renderer placed the marker at Y=400. The normal post-layout path propagates embedded frame geometry, but the snapshot path intentionally skipped that path to suppress script and plugin callbacks.

The correction calls `UpdateGeometry()` for local child views before their recursive layout pass. It preserves the script/observer restrictions and excludes plugin callbacks. The targeted rerun produced Y=400 during the blocking script and a reconciliation difference fraction of 0.0. See the [failed run](gutter-tree-parent-geometry-before-runner.log), [failed busy screenshot](gutter-tree-parent-geometry-before-hover-busy.png), [live reference](gutter-tree-parent-geometry-before-hover-reference.png), [successful rerun](gutter-tree-parent-geometry-after-runner.log) and [measurements](gutter-tree-parent-geometry-after-results.json).

### Method

Each geometry fixture first records idle normal and hover states, then starts an uninterrupted 25-second JavaScript loop. QMP sends hover, press, cross-axis exit and release inputs. Iframe cases also send wheel-down/up input while the script remains busy. Framebuffer pixels establish the available text width and the position of a marker after the text. The tests require that the script has not ended and that the renderer has published no frame during the busy input sequence.

The container query changes text from 24px/32px to 28px/36px and changes a two-column Grid into one column. Thus the test covers a query result that changes layout, rather than only a color or scrollbar overlay. The iframe fixtures cover network-loaded content, srcdoc, a nested child frame and a child whose width changes when the parent scrollbar expands. Idle checks detect speculative repaint loops and spurious ResizeObserver delivery.

After the script ends, Blink reports the accepted client dimension. The tests compare the affected viewport crop before and after renderer reconciliation. Components differing by more than two color levels count toward a 2% failure threshold. These screenshots and publication counts establish behavior during a blocked renderer; they do not measure optical latency or guarantee a frame rate.

The `frame-policy` fixture checks same-origin access, cross-origin rejection using a second HTTP port, opaque sandbox origins for network and srcdoc documents, blocked script execution, inherited CSP and removal of a frame with a pending load. It then navigates the main frame away to exercise child teardown. This is targeted regression coverage, not a complete browser security conformance suite.

### Measured reflow results

All five new geometry cases passed on the final EFI. Values follow normal → hover → pressed order. Marker Y is a framebuffer coordinate including the 80-pixel browser toolbar.

| Case | Text-column width (px) | Text-end marker Y (px) | Busy renderer publications | Reconciliation difference |
| --- | --- | --- | ---: | ---: |
| Size container query | 476 → 420 → 380 | 368 → 468 → 504 | 0 | 0.0 |
| Network iframe | 476 → 420 → 380 | 368 → 468 → 504 | 0 | 0.0 |
| srcdoc iframe | 476 → 420 → 380 | 368 → 468 → 504 | 0 | 0.0 |
| Nested iframe | 476 → 420 → 380 | 368 → 468 → 504 | 0 | 0.0 |
| Parent scrollbar resizing iframe | 1256 → 1200 → 1160 | 368 → 400 → 400 | 0 | 0.0 |

Every case also recorded zero scene publications during the one-second idle check. Iframe wheel input moved the text-end marker upward by 80 pixels during the blocking script and restored its position on the opposite wheel input. The topology fallback fixture retained the initial 476-pixel width while busy, applied 420 pixels after resumption and preserved the rebuilt inner scroller's 40-pixel offset.

- [Container-query measurements](gutter-tree-scroll-gutter-container-scroll-gutter-results.json).
- [Network iframe measurements](gutter-tree-scroll-gutter-iframe-scroll-gutter-results.json).
- [srcdoc measurements](gutter-tree-scroll-gutter-srcdoc-scroll-gutter-results.json) and [pressed-state screenshot](gutter-tree-scroll-gutter-srcdoc-input-gutter-pressed-busy.png).
- [Nested iframe measurements](gutter-tree-scroll-gutter-nested-frame-scroll-gutter-results.json).
- [Parent scrollbar measurements](gutter-tree-scroll-gutter-parent-frame-scroll-gutter-results.json) and [busy hover screenshot](gutter-tree-scroll-gutter-parent-frame-input-gutter-hover-busy.png).
- [Topology fallback measurements](gutter-tree-scroll-gutter-fallback-scroll-gutter-fallback-results.json).
- [Origin, sandbox, CSP and detach log](gutter-tree-frame-policy.log).

### Build and reproduction

Build and link from the project directory:

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
git -C ../../work/chromium-build/chromium apply --reverse --check "$PWD/patches/blink-build.patch"
git diff --check
```

The build, link, binary check, reverse patch check, Python syntax compilation and JavaScript fixture syntax checks passed. Full Chromium WPT/browser suites were not run. Runtime tests use the UEFI image. Iframe-specific network fixtures use HTTP; the existing live-site regressions use HTTPS.

Start the primary fixture server in a separate terminal:

```sh
python3 tests/redirect-server.py
```

The origin test also needs the same handler on a second port, in another terminal:

```sh
python3 - <<'PYTHON'
import runpy
from http.server import ThreadingHTTPServer
handler = runpy.run_path('tests/redirect-server.py')['Handler']
ThreadingHTTPServer(('127.0.0.1', 8766), handler).serve_forever()
PYTHON
```

Run QEMU cases sequentially:

```sh
for scenario in frame-policy scroll-gutter-container scroll-gutter-fallback \
    scroll-gutter-srcdoc scroll-gutter-nested-frame scroll-gutter-parent-frame \
    scroll-gutter-iframe scroll-gutter scroll-gutter-root scroll-gutter-horizontal \
    scroll-layout scroll-controls scroll-style scroll-input scroll-site-input \
    scroll-tree wheel-fixture tabs-site; do
    QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO="$scenario" \
        python3 tests/browser-qemu.py || break
done
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 QEMU_TEST_GDB_PORT=12345 \
    INPUT_SCENARIO=renderer-recovery python3 tests/browser-qemu.py
```

### Final QEMU results

All 19 scenarios passed sequentially against the final EFI. Durations include boot, deliberate script blocking, input and teardown; they are not latency measurements.

| Scenario | Duration (s) | Result |
| --- | ---: | --- |
| `frame-policy` | 33.11 | Passed |
| `scroll-gutter-container` | 66.74 | Passed |
| `scroll-gutter-fallback` | 58.46 | Passed |
| `scroll-gutter-srcdoc` | 65.82 | Passed |
| `scroll-gutter-nested-frame` | 68.04 | Passed |
| `scroll-gutter-parent-frame` | 69.13 | Passed |
| `scroll-gutter-iframe` | 65.82 | Passed |
| `scroll-gutter` | 63.72 | Passed |
| `scroll-gutter-root` | 67.01 | Passed |
| `scroll-gutter-horizontal` | 68.27 | Passed |
| `scroll-layout` | 55.85 | Passed |
| `scroll-controls` | 99.57 | Passed |
| `scroll-style` | 52.86 | Passed |
| `scroll-input` | 96.52 | Passed |
| `scroll-site-input` | 52.16 | Passed |
| `scroll-tree` | 47.9 | Passed |
| `wheel-fixture` | 27.92 | Passed |
| `tabs-site` | 40.32 | Passed |
| `renderer-recovery` | 68.5 | Passed |

Regression coverage includes existing root/nested/horizontal gutter reflow, scrollbar part layout, arrow/track/RTL controls, hover/pressed/disabled appearance, keyboard input, independent sidebar/document scrolling on the live Python documentation site, sticky constraints and wheel cancellation. The live `zeroday0619.dev` scenario passed loading-input, theme interaction, scrolling and tab-switch checks. Renderer recovery passed infinite-script termination, process-monitor termination, page-fault recovery and invalid-opcode recovery.

Runner logs use `gutter-tree-<scenario>-runner.log`; serial logs use `gutter-tree-<scenario>.log`. JSON measurements and PNG screenshots use the same prefix. All final source and test hashes matched their manifests after the suite completed.


## Changed components

- `src/blink/scroll_scene.cc` and `.h`: frame-tree collection, area lookup, speculative reflow and restoration.
- `src/blink/browser_frame.cc`: local child creation, navigation, policy handling and tree-wide scroll acknowledgements.
- `src/blink/navigation.cc` and `.h`: preservation of supplied child navigation parameters.
- `patches/blink-build.patch`: snapshot lifecycle state for all local frame views.
- `tests/scroll-gutter.py`, `tests/scroll-gutter-fallback.py`, `tests/frame-policy.py`, `tests/input-scenario.py` and their HTML fixtures: QMP input and visible geometry checks, origin/policy tests and topology fallback.
