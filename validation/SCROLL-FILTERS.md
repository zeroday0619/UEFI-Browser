# Filtered asynchronous scroll composition

## Implementation and scope

The previous live YouTube scene rejected Blink's pixel-moving-filter clip
sentinel. That node is an infinite input-expansion marker. The capture now
skips the marker while retaining the corresponding effect, its local transform,
output clip and ordered filter operations. Rounded output clipping remains
outside the filter layer; descendant clips remain inside it. Reapplying the
output clip to the filter input would discard pixels needed by blur halos.

Scroll wire version 4 carries value records for grayscale, sepia, saturation,
hue rotation, invert, brightness, contrast, opacity, blur, drop shadow and
alpha-preserving color matrices. The BSP rebuilds these filters through the
existing Chromium/Skia implementation. No native Skia object, callback, picture
program or renderer pointer crosses this path. The decoder checks operation
counts, kinds, finite values, ranges, indices and ancestor-chain limits. The
startup self-test covers malformed filter records and an overlong chain in an
otherwise valid encoded scene. This is targeted boundary coverage, not a full
sandbox audit or proof that Skia is safe for every accepted combination.

The compositor computes input bounds in each effect's local coordinates on
every draw. Child output bounds include their filters before being mapped to a
parent. These conservative bounds are save-layer allocation hints, not new
clips. Uncomputable bounds retain the original unbounded hint. This reduces the
cost of applying small effects across a full viewport and follows moving
scroll/sticky descendants without waiting for Blink.

Backdrop filters, arbitrary reference/SVG filter graphs, filters creating alpha
from transparent black, perspective/3D transforms and values exceeding the
resource limits remain on the normal renderer path. This change does not
complete Chromium feature coverage or hardware acceleration.

## Candidate and commands

Candidate: `build/scroll-filter-bounds/esp/EFI/BOOT/BOOTX64.EFI`.
SHA-256: `b21f120b6410b134df6008c351d0c027430a39e4f1440eaaaadaf55f4654bf7b`.
Native compilation, EFI linking and `validate-binary.py` passed. No production
dependency was added. The default `build/blink` image was not replaced.

```sh
cp src/blink/{scroll_snapshot.h,scroll_scene.h,scroll_scene.cc,scroll_wire.cc,remote_frame.cc} \
  ../../work/chromium-build/chromium/third_party/blink/uefi/src/blink/
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
BUILD_DIRECTORY="$PWD/build/scroll-filter-bounds" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/scroll-filter-bounds" python3 validate-binary.py
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/scroll-filter-bounds \
  --https-ca build/inspector-timing-https/ca.esl --prefix scroll-filter-bounds \
  --require-cpl3 --require-no-renderer-faults --require-scroll-wire scroll-filter
```

The fixture server runs `tests/network-servers.py --directory
build/inspector-timing-https`. The native/link logs use the prefix
`scroll-filter-bounds`.

## Controlled QEMU check

The fixture combines twelve CSS filter samples, nested opacity/filter effects,
a transformed filter, rounded clipping, a sticky child, a blurred source above
the viewport and an initially offscreen blurred marker. Actual QMP wheel input
moves a nested scroller 3,200 pixels and the root 3,840 pixels while JavaScript
spins for 20 seconds. The test requires presentation acknowledgements before
comparing busy and reconciled frames and verifies that DOM scroll handlers
have not run during the busy capture.

The first test captured before all queued inputs had been presented. After
synchronizing on the compositor's post-presentation log, the remaining mismatch
was confined to the run button's renderer-owned pressed appearance. The final
test compares the entire marker region around that independent control. Both
failed runs remain in `scroll-filter-first` and `scroll-filter-synchronized`.
The pixel-difference threshold remains 0.02 of channels differing by more than
two; no filter-region threshold was relaxed.

| Measurement | Full-viewport layer hints | Effect input bounds |
| --- | --- | --- |
| Image/build | `build/scroll-filter` | `build/scroll-filter-bounds` |
| Prefix | `scroll-filter-baseline` | `scroll-filter-bounds` |
| First changed scroll capture | 135.71 ms | 37.41 ms |
| Logged input-poll gaps above 50 ms during the long task | 90 | 0 |
| Median of logged gaps | 115.4015 ms | No samples above threshold |
| Completed asynchronous scroll commands | 88 | 88 |
| Fixture result | Passed, 48.76 s | Passed, 48.87 s |

These are sequential single runs under QEMU/KVM with four virtual CPUs and
one test worker. The capture timing includes QMP and polling overhead and is
not optical latency. Thresholded logs are not an unbiased latency distribution;
zero logged samples does not mean zero latency. The test waits for a fixed
20-second script, so total scenario duration is not a throughput benchmark.
See `scroll-filter-comparison.json` and both scenario result JSON files.

Both builds reported CPL3, six wire publications, a passing wire self-test and
no unexpected renderer faults. All stationary filter comparisons were exactly
equal. After reconciliation, the nested region's changed-channel fraction was
0.0000776; all other compared filter/marker regions were exactly equal.

The optimized busy frame was visually inspected for the clipped blur, fixed
filter samples, transformed shadow and root marker. Comparing corresponding
full frames across the two builds found zero changed channels initially and
after reconciliation, six channels differing by one after the first wheel and
nested burst, and one channel differing by one after the root burst. See
`scroll-filter-image-comparison.json`.

## Regressions

`scroll-filter-regression-suite.json` records passing `scroll-affine`,
`scroll-affine-controls`, `scroll-style`, `scroll-gutter-parent-frame`,
`scroll-state-iframe` and `mse-batch` scenarios. This suite used two isolated
workers; it is correctness evidence, not a performance comparison. All six
reported CPL3 execution, validated wire publications, passing startup wire
checks and no unexpected renderer faults.

`scroll-filter-sources.json` records source, test, baseline and image hashes.
The five production files match the native source mirror. Reversing
`scroll-filter-change.patch` in an isolated directory reconstructed each saved
pre-filter source exactly. These files preserve the existing affine/input-order
work and do not stage or commit the working tree.

## Live YouTube retry

The full `scroll-filter-youtube` run completed in 500.10 seconds on the same
candidate under QEMU 11.1.2 (Debian `1:11.1.2+ds-3`), KVM, four virtual CPUs
and 4 GiB RAM. `scroll-filter-youtube-suite.json` records exit code 1, CPL3
execution and no unexpected renderer fault. This is a failed playback
acceptance test, not a completed YouTube implementation.

```sh
YOUTUBE_LONG=1 YOUTUBE_INSPECT=1 YOUTUBE_PLAYBACK_PROBE=1 \
YOUTUBE_CONSOLE_TIMEOUT=120 QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 \
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --build-directory build/scroll-filter-bounds \
  --https-ca build/inspector-timing-https/ca.esl --prefix scroll-filter-youtube \
  --require-cpl3 --require-no-renderer-faults youtube-observe
```

The page initialized VP9/Opus through MSE. The 90-second screenshot was visually
inspected and showed the Big Buck Bunny video, Korean YouTube controls and page
content, with missing recommendation thumbnails. No `clip-filter` rejection
occurred. Before opening DevTools, the page published 367 validated scroll
scenes with eligibility `none`; the complete run published 554 scenes, including
the inspector. The live page still reported a global blocking-wheel fallback
because a blocking listener had no captured regional hit data. Successful
scene capture does not prove asynchronous wheel dispatch everywhere on this
page.

Eleven Console-mediated player samples reported no media error, advancing
video frame counts, a stable paused position, successful resume and a completed
seek to 120 seconds. These typed commands do not verify pointer operation of
every live player control. The unchanged real-time acceptance interval is
0.9–1.1 times the page clock:

| Interval | Page elapsed time | Media progression | Ratio | Result |
| --- | --- | --- | --- | --- |
| Initial playing | 20.262100 s | 15.659417 s | 0.772843 | Failed |
| Paused | 10.057000 s | 0 s | 0 | Passed |
| Resumed | 20.535600 s | 19.760021 s | 0.962232 | Passed |
| After seek | 15.597300 s | 15.597167 s | 0.999991 | Progressed |

See `scroll-filter-youtube-youtube-observe-youtube-playback-results.json` for
all samples and checks. Live-site runs are not controlled A/B comparisons.
The initial failing interval prevents claiming sustained real-time playback.

The AC97 capture contains 397.759 seconds of 48 kHz, stereo, signed 16-bit PCM.
Every-sixteenth-frame sampling found nonzero samples in both channels, with
peaks 32,371 and 32,767. The RIFF/data sizes remain zero after the harness stops
QEMU following the failed assertion; the analysis validates the format and uses
actual available PCM length. `scroll-filter-youtube-audio.json` records hashes
and measurements. Physical audibility and audiovisual synchronization were not
verified. This run also does not establish graceful renderer teardown.

`scroll-filter-youtube-pre-inspector-costs.json` records thresholded diagnostics
before DevTools. Scene capture now succeeds and exposes additional renderer
work: logged `scroll_us` has median 66.431 ms and logged encoding totals have
median 141.673 ms. These overlapping, threshold-selected samples are not
exclusive CPU accounting or an unbiased frame-time distribution. They identify
capture/encoding work for further profiling; they do not establish the sole
cause of media starvation. Console readiness took 23.776 seconds, including a
21.998-second debugger-enable phase.
