# Autonomous queued Opus playback

The optional `ENABLE_MEDIA_SERVICE=1` path now retains compressed MSE audio
packets in BSP-owned queues. The separate CPL3 media process decodes them while
Blink/V8 is blocked. The BSP applies Chromium's original `AudioDiscardHelper`
and submits copied PCM to the AC'97 streaming service. Network delivery and
ChunkDemuxer still run in the renderer, so execution remains independent only
while retained compressed data is available. Video decoding and presentation
remain renderer-dependent.

## Implementation and bounds

- `runtime/firmware_media.h` defines fixed-width packet metadata and status.
- `src/process/media_opus_queue.h` owns copied compressed data, round-robin
  dispatch, PCM backpressure, cancellation and stream status. Each of eight
  decoder streams holds at most 512 queued packets and 4 MiB of queued bytes.
  One additional packet may be in flight. The codec packet limit remains
  65536 bytes and decoded output remains bounded to 5760 mono/stereo frames.
- `src/blink/firmware_audio_timeline.cc` applies the existing Chromium discard
  and timestamp logic to validated metadata, converts finite PCM samples and
  retains no renderer pointers. Metadata bounds include timestamps, duration,
  discard padding, initial discard and origin; complete-discard markers are
  represented explicitly rather than transporting internal sentinel values.
- `src/audio/ac97.c` validates that an attached device handle belongs to the
  renderer owner and represents a streaming device. Its submitted-frame count
  preserves continuity across decoder configuration changes.
- `src/blink/media_source_audio.inc` fills the queue and polls status without
  waiting for individual decode completions. Chromium's Opus decoder exposes
  its service handle and parsed initial-discard value through the maintained
  `blink-opus-service.patch`. Disabled-service builds retain the local decoder.
- `src/blink/media_source_player.inc` closes/cancels the codec before resetting
  the device during seek. This prevents an older queued completion from
  writing into the new playback sequence.

The BSP bounds all copied input/output sizes and keeps handle generations and
transaction ownership private. A renderer cannot collect the queue's internal
decode transaction as a synchronous response. Invalid queued commands never
retain user pointers. Decoder close frees retained packets and timeline state;
renderer teardown first joins the media AP, then releases queues and device
buffers. Pending synchronous codec operations briefly take priority, with a
four-millisecond expiration so an abandoned retry cannot stop queue scheduling
indefinitely. Service failure invalidates handles and releases queued data.

This adds timestamp processing to the trusted BSP. Its inputs are bounded and
its decoder output is copied, but this work is not a complete audit of the
trusted runtime or proof of the overall sandbox goal. Quotas are per renderer
service, not per web origin. The prototype does not provide full audio/video
synchronization while video presentation is blocked.

## Controlled QEMU comparison

`tests/input-pages/media-queue.html` uses a reproducible 20-second Opus fixture,
starts playback, blocks JavaScript for six seconds, then checks pause, resume
and seeking to 12 seconds. `tests/make-mse-audio-fixtures.py` generates the
fixture with identifiable stereo tones. The host observes captured PCM during
the blocked interval. The scenario additionally requires service decode-count
markers within that interval, ruling out success based solely on old decoded
PCM. The completed page was visually inspected.

The before and after runs used four KVM vCPUs, one suite worker and the same
fixture, sequentially. Their retained reports are `media-queue-before-suite.json`
and `media-queue-after-suite.json`.

| Measurement | Previous synchronous service | Queued service |
| --- | ---: | ---: |
| Interior observation interval | 5.79947 s | 5.81876 s |
| Non-silent PCM / wall-clock ratio | 0.33198 | 0.99861 |
| Maximum non-silent PCM gap | 3.87194 s | 0.04067 s |
| Continuity gate | Failed | Passed |

The new service reports decode-count markers 200, 300 and 400 between the
JavaScript begin/end markers. The page reports 6.0056 seconds elapsed and
6.01544 seconds of media advancement, zero pause movement, successful resume
and a post-seek position of 12.29158 seconds with no media error. This is
host-captured PCM evidence, not physical speaker measurement or audiovisual
synchronization evidence.

The first queue run measured continuous PCM but failed in the host harness
because a branch-local `json` import had not been initialized. That attempt
remains in `media-queue-first-suite.json`; the successful repeat uses the fixed
harness. The same first image passed MSE audio/PCM checks in 42.81 seconds and
zero-rate/resume in 26.15 seconds. The diagnostic boundary image subsequently
passed MSE audio in 44.24 seconds and the six-second queue scenario in 32.36
seconds.

## Boundary and cancellation coverage

`src/process/media_queue_probe.h` tests invalid pointers, non-streaming and
duplicate device handles, invalid timestamp/discard metadata, packet and byte
quota exhaustion, queue cancellation, stale handles and end-of-stream output.
It fills a paused device's PCM ring before applying pressure to the encoded
queue. A valid Opus packet padded to 65536 bytes exercises the independent
4 MiB limit. Closing the full queue, resetting the device and opening a new
stream must leave the device empty until new data is submitted.

The probe's reported check count includes polling assertions and therefore
varies with scheduling. The runner requires its completion marker and minimum
count with `--require-media-queue-probe`; it does not interpret the variable
count as that many distinct test designs. The existing 65 broker checks and
32 synchronous media checks remain separate.

`media-queue-recovery-suite.json` passes the existing 14 MSE boundary cases in
33.97 seconds and forced renderer termination/restart in 47.61 seconds. The
termination occurs during the six-second script with compressed work queued.
The BSP releases 219260 queued bytes, and the restarted renderer passes the
continuity, pause/resume and seek checks. This case intentionally triggers a
renderer fault. Queue-probe completion counts are 97 for the boundary case
and 98/99 before and after recovery.

The final production candidate, `build/media-queue-final`, has EFI SHA-256
`9bb60f92ae09525cb9f05e7f878dc904d95c33c43b1abb1474278a96aaa33d4e`.
Its six-second blocking-script test passes in 31.32 seconds with 600 production
packets decoded, CPL3 execution and no renderer fault. The observed non-silent
PCM/wall-clock ratio is 0.99831 with a maximum signal gap of 40.68 ms; decode
markers 200, 300 and 400 occur during the block. The disabled-service candidate
passes the original MSE audio and PCM test in 44.83 seconds. Source and image
identities are retained in `media-queue-sources.json`; the production changes
relative to the previous synchronous service are in `media-queue-implementation.diff`.

## Reproduction

Rebuild the patched native Chromium archive before the EFI link. The current
checkout uses:

```sh
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
ENABLE_MEDIA_SERVICE=1 BLINK_SOURCE_POSITIONS=eager \
  BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/media-queue" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/media-queue" python3 validate-binary.py
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/media-queue \
  --https-ca build/inspector-timing-https/ca.esl --prefix media-queue-after \
  --require-cpl3 --require-no-renderer-faults --require-media-service mse-queue
```

For a fresh checkout, first use the project's normal `prepare-blink.py`
workflow to apply patches and copy sources. Add `ENABLE_BROKER_PROBE=1` for the
boundary build and `--require-broker-probe --require-media-queue-probe` for its
suite. `mse-queue-recovery` deliberately terminates the renderer while queued
audio and a long script are active; do not combine that negative test with
`--require-no-renderer-faults`.

## Live YouTube observation

`media-queue-youtube-suite.json` records a 737.00-second run of the final
production candidate. The renderer remained at CPL3 without an unexpected
fault and the media service decoded 30900 packets. Initial playback advanced
at 0.95569 of wall time, pause held exactly, resumed playback advanced at
0.96011 and the post-seek interval at 1.00022. Seeking to 120 seconds completed
without a media error. The saved visible-page screenshot was inspected and
shows a decoded Big Buck Bunny frame and the populated YouTube page.

The run exits with failure because the final independent visible-tab
measurement never returned. Its result object has no completed checks map.
The existing test registered a visibility listener through Console and
switched tabs without waiting for evaluation to complete. The next test now
requires a listener-installed acknowledgement before the tab switch and saves
timeout screenshots; the failed run alone does not prove that this race caused
the missing result. Captured output contains 613.251 seconds of stereo 48 kHz
PCM, preserved as raw and normalized WAV files in validation. Neither this
capture nor the earlier control measurements prove continuous audiovisual
synchronization or full-video playback.

The [configuration-transition follow-up](MEDIA-CONFIG-TRANSITION.md) reproduces
and fixes cancellation of old compressed audio when the channel configuration
changes. Its image and tests are separate from the live candidate above.

The service remains disabled by default. The default EFI image has not been
replaced. Remaining work includes autonomous demux/network replenishment,
video decoding/presentation, complete live-player validation, broader codec
inputs, resource-pressure behavior and per-origin service isolation.
