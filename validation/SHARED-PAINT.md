# Shared viewport and scroll painting

## Implementation

The previous frame loop called `WebView::PaintContent` for the visible viewport
and then painted the same document again for the scroll scene. The live test
in `SCROLL-ENCODING.md` retained substantial synchronous paint and capture time
after ordinary picture serialization had been divided into scheduler tasks.

The renderer now obtains the viewport record and scroll metadata from one
Blink paint artifact. `ViewportSnapshot` uses the layout view's local border-box
property state and Chromium's existing `PaintArtifact::GetPaintRecord` culling
path. An explicit viewport clip matches the original `WebView::PaintContent`
boundary. The record is created before scroll-property eligibility checks and
before speculative scrollbar reflows, so rejecting an asynchronous scene still
leaves a complete visible frame. If no shared record can be captured, the
original viewport path remains available.

The viewport and asynchronous scene retain the same paint epoch. Their immutable
records stay inside CPL3, and scene publication still uses the existing validated
wire protocol. No decoder checks, process boundaries or resource limits were
removed. The existing cooperative encoding, cancellation and publication order
remain in place.

Capture failures now retain the first concrete reason: nontranslation transform,
main-thread repaint requirement, scroll snap, clip path/filter, filter, backdrop
filter, resource/picture limit, scrollbar ownership/properties, unavailable frame
set, empty scene, image read or font serialization. This distinguishes the first
rejection; it does not enumerate every unsupported property in a document.
The named log describes synchronous capture; a later encoding failure still
discards the scene through the existing asynchronous completion path.

## Build and pixel comparison

The tested image is `build/shared-paint/esp/EFI/BOOT/BOOTX64.EFI`, SHA-256
`ff595352372042593866e91772a235583826db7732e5fae8f04e90641030e439`.
Native compilation, EFI linking and `validate-binary.py` passed. The native
source mirror matches all four changed rendering files. Reversing
`shared-paint-change.patch` reconstructs the saved pre-change files; their hashes
match the previous `scroll-encode-sources.json` manifest.

The reference uses `build/scroll-encode-final`, SHA-256
`350172e9b6c6841dd1353b9ad79bcb44a81c076096c75961b6335418466bfed9`.
`paint-snapshot.html` combines overlapping translucent content, rotated/scaled
text, clip paths, blur, a gradient mask, SVG, an iframe, sticky positioning and
an offscreen box shadow extending into the viewport. QMP input changes styles
and scrolls the root by 640 pixels.

Both QEMU runs passed in CPL3 without an unexpected renderer fault. All RGB
channels in the 1,280 × 720 page region were identical for the initial, changed
and scrolled screenshots. The top 80 native-toolbar rows were excluded.
`shared-paint-pixels.json` records zero changed channels in all three comparisons.
The initial image was also visually inspected. This checks the fixture's
rendering cases, not complete CSS or web-platform conformance.

```sh
cp src/blink/frame_loop.{cc,h} src/blink/scroll_scene.{cc,h} \
  ../../work/chromium-build/chromium/third_party/blink/uefi/src/blink/
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
BUILD_DIRECTORY="$PWD/build/shared-paint" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/shared-paint" python3 validate-binary.py
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/shared-paint \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix shared-paint-candidate --require-cpl3 \
  --require-no-renderer-faults paint-snapshot
```

The commands use the existing configured Chromium checkout. Start the fixture
server with:

```sh
python3 tests/network-servers.py --directory build/inspector-timing-https
```

The matching reference command uses
`--build-directory build/scroll-encode-final --prefix shared-paint-reference`.
The generated PPM files under the two `build/state-workers` directories retain
the raw pixels used by the comparison.

## Regression checks

`shared-paint-regression-suite.json` records passing `scroll-encoding`,
`scroll-tree`, `scroll-gutter-parent-frame`, `scroll-state-iframe` and `mse-batch`
scenarios. Every case reported CPL3 execution, successful scroll-wire self-tests
and publications, and no unexpected renderer fault. The tree test observed its
first nested scroll at 19.74 ms during a 12-second JavaScript task and measured
zero pixel difference after renderer reconciliation. The queued-paint MSE
interval advanced at 1.00089 times elapsed time without a media error.

The 180-panel fixture completed 34 animation callbacks and 182 timer callbacks
in 12.117 seconds, with an 8.2 ms median timer interval and a 279.9 ms 95th
percentile. Mouse delivery, typed input, tab hide/return and multi-slice encoding
passed. This single run does not establish a browser-wide frame-rate or tail
latency improvement.

```sh
SCROLL_ENCODING_REQUIRE_SLICES=1 QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 \
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --build-directory build/shared-paint \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix shared-paint-regression --require-cpl3 \
  --require-no-renderer-faults --require-scroll-wire \
  scroll-encoding scroll-tree scroll-gutter-parent-frame scroll-state-iframe mse-batch
```

## Live YouTube validation

The final run used QEMU 11.1.2 (Debian `1:11.1.2+ds-3`), KVM, four vCPUs and
4 GiB of guest memory. `shared-paint-youtube-suite.json` records a 487.07-second
run in CPL3 without an unexpected renderer fault. The 180-second screenshot
contains the video and YouTube UI. Console readiness took 16.71 seconds, and
the original typed probe completed 11 samples covering pause, resume and seek
to 120 seconds without a media error.

The full test failed with exit status 1 because both real-time gates failed:

| Interval | Wall time | Media advancement | Ratio |
| --- | --- | --- | --- |
| Playing | 20.506 s | 14.033 s | 0.684 |
| Paused | 10.625 s | 0 s | 0 |
| Resumed | 20.013 s | 14.635 s | 0.731 |
| After seek | 16.517 s | 13.141 s | 0.796 |

The real-time gate remains 0.9–1.1. The video tab is visible during observation
waits and hidden during inspector commands. The harness stops QEMU after the
failed assertion, so this run does not verify graceful teardown. The live site
and server responses are uncontrolled; these ratios do not establish a causal
speedup over preceding runs.

Before the inspector opens, the scene changes from eligibility 0 to 20,
`nontranslation-transform`. This identifies the first rejected property category
behind the previous generic code 10. The visible viewport still renders through
Blink, but this page cannot use the custom asynchronous scroll scene in that
state. General transform representation and hit testing remain necessary.

`shared-paint-youtube-costs.json` preserves the slow-task breakdown. Only 72 frame
records exceed the 50 ms reporting threshold, with a maximum of 1.178 seconds.
The shared-paint metric now includes document painting and viewport conversion;
the residual scroll metric covers metadata and reflows. The component boundaries
and thresholded samples differ from the former double-paint path, so their sums
must not be used as an overall CPU-time or throughput comparison. V8 debugger
enable still occupies 16.64 seconds.

`shared-paint-youtube-audio.json` records 370.619 seconds of 48 kHz, 16-bit stereo
PCM with nonzero samples in both channels. The QEMU capture has zero RIFF/data
length fields. Its fixed PCM format was validated, and a separate
`build/state-workers/shared-paint-youtube/0/audio-playable.wav` was written and
checked byte-for-byte against the original PCM. The raw capture is preserved.
This verifies output data, not physical audibility, continuous playback or
audio/video synchronization.

```sh
YOUTUBE_LONG=1 YOUTUBE_INSPECT=1 YOUTUBE_PLAYBACK_PROBE=1 \
YOUTUBE_CONSOLE_TIMEOUT=120 QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 \
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --build-directory build/shared-paint \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix shared-paint-youtube --require-cpl3 \
  --require-no-renderer-faults youtube-observe
```

## Remaining scope

Blink painting and layout remain synchronous. The asynchronous compositor still
cannot represent general transforms, filters and several scroll-dependent
effects. Visible rendering continues through Blink when that cache is rejected.
Complete Chromium compatibility, sustained real-time YouTube playback, full
sandbox isolation and hardware GPU acceleration remain unverified. The default
`build/blink` image is unchanged.
