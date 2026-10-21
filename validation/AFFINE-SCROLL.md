# Affine scroll composition and ordered input

## Implementation

The previous live YouTube run rejected its scroll scene at the first
nontranslation transform. `ScrollTransform` now retains all six 2D affine
coefficients, including the CSS transform origin. The compositor multiplies
ancestor matrices for painting and uses their inverses for rectangle and rounded
clip hit testing. Scrollbar hover, pressed state, thumb dragging, arrow repeat
and track paging use scrollbar-local coordinates. Sticky offsets remain in
layout coordinates before the ancestor matrix is applied. Reflections and
singular transforms are represented without native object references.

Scroll wire version 3 transports the four new linear coefficients. Decoding
rejects nonfinite/out-of-budget coefficients and composed matrices. Synthetic
thumb transforms must be direct translation children of their scrollbar.
Dynamic transforms and their inverses are checked again before painting or hit
testing. Singular transforms are accepted but have no drawn or hittable area.
Existing private mailbox copying, graph/size/depth checks, compressed-tile
validation and resident cache limits remain in place. Perspective/3D transforms
remain ineligible for asynchronous capture.

The new regression exposed an existing input-order defect. Pointer events were
queued as scheduler tasks, while later absolute scroll commands changed Blink
geometry immediately. A wheel event originally inside a blocking listener then
hit a different node after the scroll, adding 80 pixels after JavaScript resumed.
`scroll-affine-input` records the failure: the compositor published 1,467 pixels
but Blink ended at 1,547 pixels. Pointer, keyboard, absolute-scroll and scrollbar
state updates now share one renderer input queue. The AP leaves commands in its
bounded ring when that queue is full. Navigation still invalidates queued input
through the existing generation mechanism.

## Build and identity

The candidate is `build/scroll-affine-order/esp/EFI/BOOT/BOOTX64.EFI`, SHA-256
`75f5d91fe4c6a7416ab4cbe2aaeca4c4f7af75f022ac09c1d8530352ce8d5361`.
Native compilation, EFI linking and `validate-binary.py` passed.
`scroll-affine-sources.json` records the changed production sources, saved
pre-change sources and image hashes. The native source mirror matches those
production files. `scroll-affine-change.patch` isolates this change from the
pre-existing working tree.

```sh
cp src/blink/{scroll_snapshot.h,scroll_scene.h,scroll_scene.cc,scroll_wire.cc,browser_frame.cc,remote_frame.cc} \
  ../../work/chromium-build/chromium/third_party/blink/uefi/src/blink/
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
BUILD_DIRECTORY="$PWD/build/scroll-affine-order" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/scroll-affine-order" python3 validate-binary.py
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/scroll-affine-order \
  --https-ca build/inspector-timing-https/ca.esl --prefix scroll-affine-order \
  --require-cpl3 --require-no-renderer-faults --require-scroll-wire \
  scroll-affine scroll-tree scroll-style scroll-gutter-parent-frame \
  scroll-state-iframe mse-batch
```

The fixture server is `tests/network-servers.py --directory
build/inspector-timing-https`. No dependency was added and the default
`build/blink` image was not replaced.

## Affine fixture

`scroll-affine.html` contains a rotated/scaled/skewed nested scroller, rounded
clip, sticky header, custom scrollbars, a blocking wheel listener, a reflected
rectangle, a collapsed transform and a transformed iframe. Actual QMP input
runs while JavaScript spins for 20 seconds. The test checks wheel targeting,
sticky pixels, hover/pressed colors, thumb dragging and final DOM reconciliation.

The ordered candidate passed in 48.86 seconds with CPL3 execution, seven wire
publications and no unexpected renderer fault. The first changed scroll capture
arrived in 14.87 ms. This measures QMP-to-framebuffer observation on one run; it
is not optical latency or a distribution. The final nested offset was 1,467
pixels in both compositor and Blink. The root offset was 80 pixels. The
transformed blocking wheel region did not scroll asynchronously.

The changed-channel fraction, using an absolute channel difference greater than
two, was 0.009674 for the nested region and 0.017353 for the iframe region. Both
passed the existing 0.02 reconciliation threshold. These are approximate raster
matches, not pixel identity: cached local raster tiles transformed on the BSP
can differ from Blink's direct vector raster at edges and subpixel positions.
The busy and reconciled screenshots were visually inspected. The first attempt
also sampled a pixel covered by the cursor; that test-coordinate error is
retained in `scroll-affine-check` and was corrected before the ordering failure
was diagnosed.

The production decoder self-test covers every truncation, structural mutations,
NaN/infinity/oversized float bits in all six affine fields, affine round trips,
singular transforms, ancestor composition overflow and damaged compressed tiles.
It passed during QEMU startup. This is selected boundary coverage, not a complete
security audit.

## Regression coverage

The `scroll-affine-order` suite passed all six scenarios in CPL3 with no
unexpected renderer fault. Each run also passed the production decoder self-test.

| Scenario | Elapsed seconds | Wire publications |
| --- | ---: | ---: |
| Affine nested scroll | 48.86 | 7 |
| Nested/root sticky and scroll chaining | 48.83 | 13 |
| Native/custom scrollbar appearance | 53.37 | 7 |
| Parent-frame gutter and container-query layout | 69.68 | 9 |
| Iframe scroll-state layout | 60.26 | 9 |
| MSE packet batching during paint | 36.36 | 17 |

`scroll-affine-controls` repeats the vertical test with an explicit assertion
that the blocking wheel listener receives a cancelable event after JavaScript
resumes. It passed in 48.65 seconds; first changed capture was 18.55 ms and final
positions and pixel differences matched the earlier run. The horizontal variant
passed in 48.68 seconds. During the 20-second script, dragging changed the
horizontal offset to 400 pixels, a forward-arrow click changed it to 440, and a
backward-track click changed it to 108. Hover and pressed colors appeared before
JavaScript resumed. Blink ended at 108 pixels. The changed-channel fraction
against the reconciled frame was 0.009480.

```sh
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/scroll-affine-order \
  --https-ca build/inspector-timing-https/ca.esl --prefix scroll-affine-controls \
  --require-cpl3 --require-no-renderer-faults --require-scroll-wire \
  scroll-affine scroll-affine-controls devtools
```

The DevTools case also passed in 86.74 seconds with 179 wire publications, CPL3
execution and no unexpected renderer fault. Toolbar entry/reuse, Console DOM/CSS
mutation, debugger pause/step/resume, reopening and target detachment remain
functional with the ordered input queue.

## Live YouTube result

The full `scroll-affine-youtube` run used QEMU 11.1.2
(Debian `1:11.1.2+ds-3`), KVM, four virtual CPUs and 4 GiB RAM. It ran for
501.79 seconds and exited with failure because both real-time progression checks
failed. All 11 media samples had no media error. Video frame counts advanced,
pause held the playback position, resume advanced it, and seeking to 120 seconds
completed. The video tab was visible during measurement waits; typed Console
commands required switching to the inspector.

| Interval | Page-clock seconds | Media seconds | Ratio |
| --- | ---: | ---: | ---: |
| Playing | 20.8281 | 14.140333 | 0.678907 |
| Paused | 10.1128 | 0 | 0 |
| Resumed | 20.0575 | 14.189625 | 0.707447 |
| After seek | 15.9198 | 13.238833 | 0.831595 |

The real-time acceptance band remains 0.9–1.1. These live-site intervals are not
a controlled comparison against an earlier image, and this run does not establish
a playback-speed improvement. The renderer remained in CPL3 without an unexpected
fault. The harness stopped QEMU after the failed assertions, so this run does not
validate graceful shutdown.

Before the inspector opened, the first scroll-scene rejection was now
`24 reason=clip-filter`, after three early wire encodings. The former affine
rejection was absent. A pixel-moving filter clip and its associated effect still
require a supported rendering path before this live scene can use the asynchronous
cache. Capture does not bypass the unsupported filter. V8 debugger enabling also
remains expensive: the diagnostic reported 20.838392 seconds in the enable phase.
`scroll-affine-youtube-costs.json` retains these observations.

The 120-second framebuffer was visually inspected. It shows the Big Buck Bunny
video and Korean YouTube controls. Some recommended thumbnails and characters
remain missing. The captured audio contains 389.180021 seconds of stereo
48 kHz, signed 16-bit PCM. Sampling every 16 frames gave channel peaks of 31,861
and 32,164 and RMS levels of 2,059.27 and 2,112.97. QEMU left the RIFF/data size
fields zero; the original is retained and `audio-playable.wav` contains the
identical PCM in a finalized WAV container. `scroll-affine-youtube-audio.json`
records hashes and sampling statistics. Recorded nonzero PCM does not prove
physical audibility, continuous real-time playback or audio/video synchronization.

```sh
YOUTUBE_LONG=1 YOUTUBE_INSPECT=1 YOUTUBE_PLAYBACK_PROBE=1 \
YOUTUBE_CONSOLE_TIMEOUT=120 QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 \
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --build-directory build/scroll-affine-order \
  --https-ca build/inspector-timing-https/ca.esl --prefix scroll-affine-youtube \
  --require-cpl3 --require-no-renderer-faults youtube-observe
```

## Remaining scope

Filtered/path-clipped and perspective/3D scenes still require renderer fallback.
Local cached pixels do not exactly reproduce direct vector rasterization under
arbitrary transforms. Long JavaScript, layout, paint and individual Skia calls
remain synchronous; media tasks are not independently preemptive. Full Chromium
conformance, hardware GPU execution and a complete sandbox audit remain outside
the evidence established by this change. All nine implementation goals remain
active. No files were staged or committed.
