# Reuse of immutable drawing records

## Change and scope

`src/blink/scroll_scene.cc` avoids allocating a new `DrawRecordOp` wrapper when a
captured group contains at most one nonempty drawing and every visible chunk
has the exact target property state. The pending capture retains the existing
immutable `cc::PaintRecord` by reference. Its normal playback already establishes
local canvas save/restore and transform semantics. Invisible chunks are skipped.
Groups with multiple drawings, non-drawing items or differing properties retain
Chromium's `PaintChunksToCcLayer::Convert` path. The caller still applies scene
transforms, clips and effects, enforces pending-record limits and serializes
resources through the existing callbacks. No native pointer crosses the mailbox.

The earlier mapping-only experiment did not improve the controlled workload
and is superseded; see `DIRECT-RECORDS.md`. Both experiments were informed by
live capture timing, rather than assuming that all renderer time was JavaScript.
The capture diagnostic reports scrollbar generation, conversion/queueing and
total post-paint freezing. It reports captures over 25 ms; the reuse image also
reports scenes with at least 512 pictures so faster large captures remain visible.

## Build and controlled comparison

Native compilation, EFI linking and binary validation pass for
`build/scroll-record-reuse`, SHA-256
`2e30b60b7318f443c9a474d7a35312877d6eb9715d8fbdf36d72c915e51c130b`.
The binary validator reports no required unresolved symbols or hosted
syscall/sysenter instructions. This check is not a complete sandbox proof.
`scroll-record-reuse-sources.json` and `scroll-record-reuse-change.patch`
retain the build identity and delta from the diagnostic baseline. The native
source mirror matches the project source. The default EFI image is unchanged.

The existing encoding fixture now offers a `?capture=1` variant with 600 text
panels and no canvases. `SCROLL_ENCODING_CAPTURE=1` selects it through ordinary
QEMU address-bar input. Both successful runs confirm `captureLoad: true` and
`panelCount: 600`. They pass mouse, keyboard and tab hide/resume checks at CPL3
with validated wire publications and no unexpected renderer fault.

| Animation measurement | Diagnostic baseline | Record reuse |
| --- | ---: | ---: |
| Median scene freeze | 35.260 ms | 22.847 ms |
| Median conversion/queueing | 29.662 ms | 17.141 ms |
| Timer p95 | 14.2 ms | 14.2 ms |
| Maximum timer delay | 236.9 ms | 277.9 ms |
| Animation frames | 15 | 15 |

The reuse run retains 603 groups and converts 602 groups per large capture.
The comparison excludes work before the animation-begin marker and after its
result. The baseline has 12 threshold-selected capture samples; the reuse run
has 14 samples under its broader large-scene reporting condition. These are
single sequential runs, not a statistical benchmark. They support reduced
capture cost, not an overall frame-rate or input-latency improvement. The timer
maximum is higher in the reuse run. See `scroll-record-reuse-comparison.json`.

## Reproduction

Use the fixture server `python3 tests/network-servers.py --directory
build/inspector-timing-https`. After copying the changed Blink source into the
native source mirror, build and run from the project directory:

```sh
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/scroll-record-reuse" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/scroll-record-reuse" python3 validate-binary.py
SCROLL_ENCODING_CAPTURE=1 QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 \
  python3 tests/state-suite.py --cpus 4 --workers 1 \
  --build-directory build/scroll-record-reuse \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix scroll-record-reuse-capture --require-cpl3 \
  --require-no-renderer-faults --require-scroll-wire scroll-encoding
```

The Console harness moves the native cursor away from its underline-detection
region after every Console-tab click. Address input also supports the query
separator and equals key. These changes preserve the Console selection/prompt
checks and permit the controlled fixture variant. Original failed attempts and
successful reruns are retained in `DIRECT-RECORDS.md`.

Full sandbox verification, hardware GPU acceleration, complete web-platform
compatibility and sustained live YouTube playback remain open requirements.

## Regression coverage

All seven cases in `scroll-record-reuse-regression-suite.json` pass at CPL3
without unexpected renderer faults: filtered and affine scroll, scrollbar
style, parent-frame gutter reflow, scroll-state iframe, MSE batching and DevTools.
Every case reports validated wire publications and the wire startup self-test.
Five complete 1280×800 filter captures are pixel-identical to the prior
wire-slicing image, including both busy-scrolling states and reconciliation;
see `scroll-record-reuse-image-comparison.json`. The reused record path preserves
those tested output pixels. This is not broad site-fidelity or security proof.

A full live run uses `YOUTUBE_LONG=1 YOUTUBE_INSPECT=1
YOUTUBE_PLAYBACK_PROBE=1 YOUTUBE_CONSOLE_TIMEOUT=120` with prefix
`scroll-record-reuse-youtube` and the same candidate image. Its final suite and
playback-results artifacts are required before claiming playback-speed behavior.
The first run finished with a harness failure: a full-width blue focus border
was mistaken for the Console tab underline. `devtools_tab_row` now rejects blue
lines extending beyond the tab interval. Saved-frame checks reject the banner
and Elements frames while accepting the two known Console frames; see
`inspector-focus-detection.json`. The ordinary DevTools rerun passed in 89.2
seconds. This changes readiness detection, not playback acceptance thresholds.

The corrected full retry, `scroll-record-reuse-youtube-retry`, finished in
499.14 seconds at CPL3 without unexpected renderer faults. All eleven playback
samples were obtained. Initial playback advanced at 0.826 of wall time, resumed
playback at 0.858 and post-seek playback at 0.832. Pause held, seeking completed
and no media error was reported. Both real-time acceptance gates failed.
The run does not establish a live performance improvement over earlier images;
audiovisual synchronization and sustained real-time playback remain unverified.
The original source manifest retains the earlier harness hash.
