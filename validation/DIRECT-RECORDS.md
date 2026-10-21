# Scroll paint-record capture experiment

The mapping-only implementation described below was not retained. A controlled
600-panel text workload measured conversion at 28.708 ms before versus 28.636 ms
after, with the same 15 animation frames and 14.2 ms timer p95. The maximum timer
delay increased from 236.9 to 444.6 ms in these single runs. This does not support
a performance benefit. See `scroll-direct-record-capture-comparison.json`.
The experiment source remains at `build/scroll-direct-record/experiment-scroll_scene.cc`;
its image and regression evidence remain available. Follow-up work targets
redundant wrapper allocation instead of relative-property mapping.

## Measured capture cost

`scroll-capture-cost-youtube-suite.json` records a 110.81-second QEMU/KVM run
with a 90-second live YouTube observation, CPL3 execution and no unexpected
renderer fault. It is not a playback acceptance test. Image SHA-256 is
`c4883c9947a1aa7226e499a12869c04ae0aee993cb943c6684acd4d8e28c31b7`.

The diagnostic separates post-paint scene freezing into scrollbar generation,
paint-chunk conversion and the remaining graph/bookkeeping work. Among 44
successful captures exceeding 25 ms, median total freezing is 55.185 ms,
scrollbar generation is 16.570 ms and conversion is 36.209 ms. Median scene
size is 399 pictures. These threshold-selected medians are not additive CPU
accounting. The source changes and exact samples are retained in
`scroll-capture-cost-change.patch`, `scroll-capture-cost-sources.json` and
`scroll-capture-cost-attribution.json`.

## Implementation

`src/blink/scroll_scene.cc` checks every visible chunk in a group against the
exact unaliased target property state. A group containing only drawing display
items with matching states is assembled directly from immutable `DrawRecordOp`
records. Invisible chunks are skipped. Any property mismatch or other display
item uses Chromium's existing `PaintChunksToCcLayer::Convert` implementation.
Transforms, clips and effects remain applied by the scene outside these records.
The optimization removes relative-property mapping only when there is no
relative property change. It preserves recording order and does not change
wire decoding, memory limits or the sandbox boundary.

The candidate EFI image is `build/scroll-direct-record`, SHA-256
`061674759d6a11837337574bd2254245c07af05130cf5a32f737c32f0cbb603c`.
Native compilation, EFI linking and `validate-binary.py` pass. The isolated
production delta and source/image identities are in
`scroll-direct-record-change.patch` and `scroll-direct-record-sources.json`.
The default `build/blink` image remains unchanged.

## Controlled fixture and regression results

The sequential canvas-fixture comparison passes keyboard, mouse and hide/resume
checks on both images. It shows no material timer or input improvement:
95th-percentile timer delay is 16.2 ms in both runs and the maximum is 292.9 ms
before versus 291.9 ms after. See `scroll-direct-record-comparison.json`.
This fixture does not establish a live-site performance gain.

Six cases in `scroll-direct-record-regression-suite.json` pass: filtered scroll,
affine scroll, scrollbar style, parent-frame gutter reflow, scroll-state iframe
and MSE batching. They run at CPL3 with validated wire publications and no
unexpected renderer fault. Filter and affine fixtures exercise asynchronous
scrolling during 20-second JavaScript tasks. All RGB channels in five complete
1280×800 filter captures match the earlier wire-slicing candidate exactly,
including initial, nested/root busy scrolling and reconciled states. See
`scroll-direct-record-image-comparison.json`.

The initial DevTools case fails its screenshot-based Console detector. The
saved screenshot shows a valid Console, but the native pointer breaks its blue
selected-tab underline into runs shorter than the required 35 pixels. The
harness now moves the pointer to native browser chrome after clicking Console,
including retry clicks in both the ordinary DevTools and YouTube paths. The
selection and prompt predicates are unchanged. The original failure is retained.
With that harness correction, `scroll-direct-record-console-suite.json` passes
in 86.86 seconds on the unchanged candidate image. Console DOM/CSS mutation,
debugger pause/step/resume, tab reuse and target detachment pass. The test-only
delta is retained in `scroll-direct-record-cursor-fix.patch`.

## Commands

The fixture server uses `python3 tests/network-servers.py --directory
build/inspector-timing-https`. From the project directory:

```sh
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/scroll-direct-record" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/scroll-direct-record" python3 validate-binary.py
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 2 --build-directory build/scroll-direct-record \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix scroll-direct-record-regression --require-cpl3 \
  --require-no-renderer-faults --require-scroll-wire \
  scroll-filter scroll-affine scroll-style scroll-gutter-parent-frame \
  scroll-state-iframe mse-batch devtools
```

These commands use the current native source mirror. The initial regression
record predates the pointer correction; the separate Console record verifies
that correction. No staging or commit was performed. Full Chromium compatibility,
sandbox completeness, hardware acceleration and sustained live playback remain
open requirements.

The candidate's separate live observation completed in 110.98 seconds at CPL3
without faults. It loaded fewer picture groups than the diagnostic baseline,
so no live-site performance comparison is inferred. The initial capture-workload
attempts failed because the harness lacked QEMU key mappings for `?` and `=`;
those mappings were added and both `*-fixed` runs completed with
`captureLoad: true` and `panelCount: 600`. Original failures are retained.
