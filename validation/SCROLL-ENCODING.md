# Cooperative scroll picture encoding

## Change and boundary

The previous frame task recorded the viewport, captured the scroll property
trees and converted every ordinary scroll picture to serialized Skia data
before returning to the renderer scheduler. The full live run in `JIT-INDEX.md`
attributed 58.4 seconds of reported frame-recording time to scroll capture and
reflow. That measurement did not distinguish Blink painting from serialization.

`ScrollSceneCapture` now freezes value metadata and reference-counted
`cc::PaintRecord` objects during the same synchronous capture as the viewport.
It clears the temporary Blink property-node maps and frame-view pointer before
returning. Later tasks convert and serialize ordinary pictures in batches with
a four-millisecond target. Each consumed paint record is released. The job
retains the previous scene while its image and font caches are consulted.

`FrameLoop` waits for encoding and viewport rasterization before publishing the
frame and its scroll acknowledgement. Hiding or stopping the frame invalidates
queued weak callbacks and releases uncommitted records. A failed encoding drops
the scroll scene without publishing a partial graph. The existing CPL3 wire
format, decoder validation and supervisor raster representation are unchanged.
Pending paint-record storage has a separate 32 MiB accounting limit in addition
to the existing serialized-resource limit and 4,096-picture cap. That accounting
does not measure all memory retained indirectly by paint records.

Custom and native scrollbar appearance capture remains synchronous. A scene
requiring speculative scrollbar-gutter or scroll-state variants finishes its
base encoding before variant capture, preserving resource deduplication,
aggregate budgeting and restoration of the live layout. Ordinary scenes take
the asynchronous path. Individual Skia calls can exceed four milliseconds;
this is a cooperative batch deadline, not a hard latency guarantee.

## Controlled comparison

The baseline was rebuilt from the current sources with only `frame_loop.cc`,
`frame_loop.h`, `scroll_scene.cc` and `scroll_scene.h` restored to their saved
pre-change contents. The current native source mirror and archive were then
restored. `scroll-encode-change.patch` preserves that source delta.
`scroll-encode-controlled-build.log`, the matching native/link logs and
`scroll-encode-sources.json` identify the build inputs and artifacts.

Both images enable `BLINK_TASK_DIAGNOSTICS=1`, use four QEMU/KVM vCPUs and run
the same fixture sequentially. The fixture animates 180 transformed panels
containing distinct canvas images and text while an eight-millisecond timer
runs. QMP input clicks a page button, types into a field, hides the tab and
returns to it. The candidate requires actual encoding continuations in its log.

| Measurement | Rebuilt synchronous baseline | Cooperative candidate |
| --- | --- | --- |
| Timer callbacks during approximately 12 seconds | 42 | 179 |
| Median timer interval | 243.8 ms | 8.2 ms |
| 95th-percentile timer interval | 313.9 ms | 296.3 ms |
| Maximum timer interval | 3,951.3 ms | 1,277.1 ms |
| Animation callbacks | 32 | 32 |
| Mouse and keyboard delivery, tab hide/return | Passed | Passed |

The candidate reported 37 encoded frames, a median total encoding cost of
40.726 ms, up to 14 slices per frame and a maximum reported slice of 5.607 ms.
The comparison improves scheduling opportunities but does not demonstrate a
higher animation rate. The tail remains long. Timer measurements include the
tab hide/return cycle; the mouse duration includes host movement, sleeps and
polling. Neither measurement establishes optical input latency. These are one
run per image, not a statistical performance distribution across runs.

`scroll-encode-comparison.json` and the `scroll-encode-controlled-before` and
`scroll-encode-controlled-after` files retain raw results. The baseline image is
`44e1fa9da47391bc6221e145d1bd8d5bc2c963df486ad9a8ac8efa4ed4e52801`.
The final candidate is
`350172e9b6c6841dd1353b9ad79bcb44a81c076096c75961b6335418466bfed9`.
An earlier comparison against the existing `build/jit-index` image is retained
under `scroll-encode-before` and `scroll-encode-after`; the table uses the
reconstructed comparison above.

```sh
SCROLL_ENCODING_REQUIRE_SLICES=1 QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 \
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --build-directory build/scroll-encode-final \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix scroll-encode-controlled-after \
  --require-cpl3 --require-no-renderer-faults --require-scroll-wire \
  scroll-encoding
```

The fixture server must use the corresponding trust store:

```sh
python3 tests/network-servers.py --directory build/inspector-timing-https
```

## Regression evidence

Native compilation, EFI linking and static binary validation passed for the
initial `build/scroll-encode` image. Static binary validation also passed for
`build/scroll-encode-final`; its native source mirror matches the working files.
`scroll-encode-basic-suite.json` records
passing `scroll-tree`, `scroll-style`, `scroll-gutter-parent-frame`,
`scroll-state-iframe` and `mse-batch` scenarios. All executed in CPL3, published
validated scroll wire data and had no unexpected renderer fault. The tree test
observed the first nested scroll at 15.98 ms during a 12-second JavaScript task
and found zero pixel difference after reconciliation. The MSE queued-paint
interval advanced at 1.00046 times elapsed time without a media error.

The final image was relinked after restoring the native build from the controlled
baseline. Its rendering sources match the initial candidate. The final comparison
passes the same wire and CPL3 gates. The fixture screenshot was visually inspected
for rendered canvas panels, text and the typed input.

## Live YouTube result

`scroll-encode-youtube-suite.json` records a 497.94-second run of the final
candidate with CPL3 execution and no unexpected renderer fault. The 180-second
screenshot contains the Big Buck Bunny video and the YouTube page. Console
readiness took 22.77 seconds. The typed Console probe collected 11 samples and
completed pause, resume and a seek to 120 seconds without a media error.

The test exited with status 1 because both real-time playback gates failed:

| Interval | Wall time | Media advancement | Ratio |
| --- | --- | --- | --- |
| Playing | 20.872 s | 12.047 s | 0.577 |
| Paused | 11.450 s | 0 s | 0 |
| Resumed | 20.822 s | 12.878 s | 0.618 |
| After seek | 15.320 s | 11.575 s | 0.756 |

The video tab was visible during observation waits and hidden while commands
were typed in the inspector. The existing real-time gates remain 0.9–1.1.
Audio/video synchronization and continuous real-time playback are not verified.
The harness stopped QEMU after the assertion failure; this run does not establish
graceful renderer teardown. It is not a controlled comparison against earlier
live-site runs.

Before the inspector opened, the scroll eligibility log changed from 0 to 10
and no multi-slice/slow encoding report appeared. The later log contains 161
encoding reports without tab identifiers, so they cannot be attributed
individually to the video or inspector. The measured fixture improvement must
not be generalized to every live YouTube frame. Existing eligibility checks
remain in place; the exact reason for the live scene's rejection still requires
instrumentation.

`scroll-encode-youtube-costs.json` accounts for 1,317 reported slow frame records:
83.85 seconds total, including 35.75 seconds of viewport painting and 40.80
seconds of scroll capture/reflow. These are inclusive elapsed times from logged
slow tasks, not exclusive CPU time or all frame work. V8 debugger enable also
occupied 20.53 seconds. Synchronous painting, capture rejection and debugger
initialization remain concrete targets for further work.

```sh
YOUTUBE_LONG=1 YOUTUBE_INSPECT=1 YOUTUBE_PLAYBACK_PROBE=1 \
YOUTUBE_CONSOLE_TIMEOUT=120 QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 \
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --build-directory build/scroll-encode-final \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix scroll-encode-youtube --require-cpl3 \
  --require-no-renderer-faults youtube-observe
```

## Remaining work

Blink lifecycle work, viewport painting, property capture, scrollbar variants
and CPL3 wire rasterization/compression still contain synchronous work. Long
JavaScript and individual image/font/Skia operations remain non-preemptive.
This change does not establish continuous real-time YouTube playback, complete
sandbox isolation, a complete Chromium port or hardware GPU acceleration. The
default `build/blink` image is unchanged.

[Shared painting](SHARED-PAINT.md) subsequently removes the duplicate viewport
and scroll paint pass. A controlled effect/iframe fixture matches the former
viewport pixels exactly. The live scene's first rejection is now identified as
a nontranslation transform; its full playback run still fails real-time gates.
