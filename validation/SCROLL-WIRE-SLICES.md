# Cooperative scroll tile encoding

## Attribution

The filtered-scroll image enabled capture on the live YouTube page, exposing
work that had previously fallen back to viewport-only rendering. A diagnostic
build separates picture conversion, serialization, image/font callbacks,
picture deserialization, tile rasterization and compression. The 90-second
`scroll-cost-youtube` observation completed in 111.94 seconds with CPL3 and no
unexpected renderer fault. It is an observation run, not a playback acceptance
test.

`scroll-cost-attribution.json` contains threshold-selected samples. Slow
picture samples spend a median 15.061 ms in serialization; first-use font
serialization reaches 440.734 ms. Slow wire-picture samples spend a median
10 microseconds loading a picture, 2.345 ms rasterizing and 8.542 ms compressing.
A single picture's accumulated compression time reaches 45.220 ms. These are
per-picture totals, not maximum individual zlib-call durations. Capture still
has substantial synchronous cost. The measurements do not justify eliminating
picture deserialization as the principal optimization.

## Implementation

`ScrollWireEncoder` retains an immutable scene and advances through scene
metadata, picture appearances and individual raster tiles. A call targets four
milliseconds or 64 steps. Metadata, one Skia tile draw and one compression call
remain indivisible. Tile bitmap and compression workspace lifetimes end before
yielding. Existing wire version 4, field order, graph limits, cumulative pixel
and tile budgets, zlib stream validation and private-copy decoding remain in
place.

`FrameLoop::EncodeWire` schedules continuations between ordinary renderer tasks.
Only complete encoded bytes proceed to viewport rasterization and publication.
The published pixels, scroll acknowledgement, scene and wire bytes belong to
the same frozen paint epoch. `RemoteFrame` copies the completed bytes instead
of rasterizing and compressing the entire scene inside publication. Completed
buffers are shared within the renderer to avoid another payload copy; only bytes
cross the fixed mailbox. Hiding/stopping a frame invalidates callbacks and
releases its uncommitted encoder. Encoding failure retains normal frame output
with no asynchronous cache.

A previous completed buffer can coexist with the next pending buffer. Writer
growth still checks runtime heap headroom and the 16 MiB message cap. The change
does not make allocation quotas complete or replace a full sandbox audit.
The synchronous `EncodeScrollScene` wrapper uses the same state machine for
callers that need a complete result. Startup self-tests additionally compare
one-step continuations with the wrapper, reject premature partial output and
verify scene ownership release after cancelling a partial multi-tile job.

## Controlled input comparison

Both builds use the same attribution diagnostics. The existing fixture animates
180 canvas/text panels for about 12 seconds while an 8 ms timer runs. QMP clicks
a page button, types into an input and hides/resumes the active tab. Runs were
sequential under QEMU/KVM with four virtual CPUs and one test worker.

| Measurement | Synchronous wire publication | Cooperative wire tasks |
| --- | --- | --- |
| Image directory | `build/scroll-cost` | `build/scroll-wire-slices` |
| Prefix | `scroll-wire-sync-control` | `scroll-wire-slices-control` |
| Timer callbacks | 178 | 1,007 |
| Median interval | 8.3 ms | 8.4 ms |
| 95th-percentile interval | 299.2 ms | 16.6 ms |
| Maximum interval | 1,413.5 ms | 309.6 ms |
| Animation callbacks | 34 | 32 |
| Mouse delivery and host polling interval | 0.663 s | 0.463 s |
| Keyboard delivery and tab hide/resume | Passed | Passed |

This improves scheduler opportunities, not demonstrated frame throughput. These
are single-run observations, not a distribution across runs or optical input
latency. The candidate logged 40 completed wire jobs, at most 67 slices per job,
a median per-job maximum slice of 5.471 ms and an overall maximum slice of
6.853 ms. The deadline is a cooperative target, not a strict four-millisecond
latency guarantee. See `scroll-wire-slices-comparison.json` for raw measurements.
Both runs passed CPL3, wire self-test, publication and no-renderer-fault checks.

## Build and reproduction

Candidate SHA-256:
`df9dcbe9269af6265bf3e54807b407bb745e84c408c499027bd9593222217ea7`.
Diagnostic synchronous baseline:
`629a457dfe021be66655cf75252666e4eec3ed96b102ca56dce86f3fff9f3f48`.
Native compilation, EFI linking and `validate-binary.py` passed. The default
`build/blink` image was not replaced and no dependency was added.

```sh
cp src/blink/{scroll_scene.cc,scroll_wire.cc,scroll_wire.h,scroll_snapshot.h,frame_loop.cc,frame_loop.h,remote_frame.cc} \
  ../../work/chromium-build/chromium/third_party/blink/uefi/src/blink/
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
BUILD_DIRECTORY="$PWD/build/scroll-wire-slices" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/scroll-wire-slices" python3 validate-binary.py
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/scroll-wire-slices \
  --https-ca build/inspector-timing-https/ca.esl --prefix scroll-wire-slices-control \
  --require-cpl3 --require-no-renderer-faults --require-scroll-wire scroll-encoding
```

The fixture server uses `tests/network-servers.py --directory
build/inspector-timing-https`. `scroll-wire-slices-sources.json` records hashes
for the seven changed production files, saved baselines, images and tests.
All seven native mirrors match. Reversing `scroll-wire-slices-change.patch` in
an isolated directory reconstructs the saved pre-diagnostic sources exactly.
No staging or commit was performed.


## Regression coverage

`scroll-wire-slices-regression-suite.json` records seven passing scenarios:
`scroll-filter`, `scroll-affine`, `scroll-style`, `scroll-gutter-parent-frame`,
`scroll-state-iframe`, `mse-batch` and `devtools`. These correctness checks used
two isolated QEMU workers; they are not performance comparisons. All seven
reported CPL3, valid wire publications, a passing startup self-test and no
unexpected renderer fault. The filter and affine fixtures compare busy and
reconciled frames during 20-second JavaScript tasks. DevTools verified native
entry, reuse, Console DOM/CSS changes, pause/step/resume and detachment.
The complete `Decoder` implementation was compared with the saved baseline and
is unchanged. Security coverage remains narrower than a full sandbox audit.


## First full live attempt and harness correction

`scroll-wire-slices-youtube-suite.json` records a failed 343.22-second full
attempt. It reached MSE VP9/Opus initialization, displayed video at 120 seconds
and opened DevTools with no unexpected renderer fault. It did not obtain the
first `YTSTATE` reply, so no playback-control rates can be inferred from this
attempt.

The retained failure screenshot shows the Elements panel. A late language
banner moved the tab strip after the harness clicked Console. The old detector
accepted blue Elements markup as a Console prompt. The harness now requires a
continuous selected-Console underline and a visible prompt before typing, and
retries the Console click at the visible tab row while Elements remains active.
The ordinary DevTools regression uses the same detector. Recorded-frame checks
reject the failure screenshot and a transitional tab frame while accepting two
settled Console frames. Antialiased unfocused prompt pixels are included in the
color predicate. See `scroll-wire-slices-console-detection.json`.

Before Inspector startup, this first attempt recorded 406 completed wire jobs
and publications. The median per-job maximum wire slice was 5.514 ms; the largest
was 57.116 ms. Slow capture samples still had median 67.398 ms and maximum
228.194 ms. The image was not changed for the harness correction. Full live
real-time behavior remains unproven by this attempt.


The corrected detector passed the ordinary QEMU DevTools scenario in 113.17
seconds (`scroll-wire-slices-console-suite.json`). Console mutation, debugger
pause/step/resume, reopening and target detachment passed with CPL3 and no
unexpected renderer fault. The detector waits through the transitional underline
animation before accepting the prompt. `scroll-wire-slices-console-fix.patch`
isolates the test correction; the source manifest retains both test hashes.

## Full live retry

The corrected harness completed `scroll-wire-slices-youtube-retry` in 494.02
seconds on the same candidate image. It obtained 11 player samples through
ordinary Console keyboard input. Video frames advanced, no media error was
reported, pause held, resume advanced and seeking to 120 seconds completed.
Initial progression was 0.810883 times the page clock and resumed progression
was 0.862176. Both fail the unchanged 0.9–1.1 real-time gate; the suite exits 1.
The interval after seeking progressed at 0.819331. The renderer ran at CPL3
without unexpected faults. The inspected 180-second frame shows video and
Korean UI, with missing recommendation thumbnails.

Before Inspector startup, 343 complete wire publications were accepted with
no capture rejection. The median per-job maximum encoding slice was 5.324 ms
and the largest was 24.844 ms. Threshold-selected capture samples still have
median 63.483 ms. These are overlapping measurements from one live run;
a separate diagnostic build overlapped initial observation but had ended
before the playback-control intervals. No controlled live-site speedup is
claimed. The full rates and timing boundaries are retained in
`scroll-wire-slices-youtube-retry-youtube-observe-youtube-playback-results.json`
and `scroll-wire-slices-youtube-retry-pre-inspector-costs.json`.

The AC97 capture contains 397.635 seconds of stereo 48 kHz PCM. Sampling every
sixteenth frame finds nonzero output in both channels with RMS 2134.90 and
2205.74. `scroll-wire-slices-youtube-retry-audio.json` retains hashes and sampling
limits. This does not establish physical audibility or audiovisual sync.
The raw WAV size fields are unfinalized after harness termination.

The Console readiness correction resolves the earlier false-positive panel
detection: this retry waited 19.942 seconds and selected row 136 after the banner
shift. It does not resolve the remaining playback-speed failure. The default
image remains unchanged.
