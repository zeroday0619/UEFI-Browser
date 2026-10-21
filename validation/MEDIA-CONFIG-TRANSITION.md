# Queued Opus configuration changes

The queued decoder originally closed its active stream as soon as
`ChunkDemuxerStream::Read` reported `kConfigChanged`. Closing correctly cancels
queued work during seek or teardown, but a format transition must preserve the
old audio. A sufficiently fast compressed read could reach the next format
before the media process decoded the preceding packets.

## Reproduction

`tests/input-pages/media-config.html` appends an eight-second mono Opus segment
followed by an eight-second stereo segment at timestamp offset 8. Both segments
contain fixed identifiable tones. The mono segment uses 440 Hz in both output
channels; the stereo segment uses 880 Hz and 1100 Hz. The page waits for the
`ended` event and checks duration and elapsed time. A separate host check
requires 7.6–8.2 seconds of each tone in QEMU's actual PCM capture.

The unchanged `build/media-queue-final` image fails the host check in
`media-config-before-suite.json`: it contains 0.0 seconds of the mono tone and
7.9 seconds of the stereo tone. The page nevertheless reports successful
completion at 16.021 seconds with no media error. The missing first segment
was replaced by timestamp-gap silence. The failed image has SHA-256
`9bb60f92ae09525cb9f05e7f878dc904d95c33c43b1abb1474278a96aaa33d4e`.

## Transition lifetime

`src/blink/media_source_audio.inc` now stops reading across the configuration
boundary and polls the existing stream until compressed work, the in-flight
packet and pending decoded PCM have reached the audio device. The subsequent
decoder attaches at the device's existing submitted-frame offset. DMA retains
the preceding samples while the next configuration starts. Seek and teardown
continue to cancel pending work and clear the configuration-wait state.

`FirmwareOpusStreamStatus.pending_frames` exposes unsent PCM through the
existing copied status response. An empty compressed queue alone is not a
drain barrier: a full audio device can retain a completed packet's PCM.
`src/process/media_queue_probe.h` explicitly fills the paused device, decodes
one more packet and requires zero queued packets with 960 pending frames before
cancellation. End-of-stream status must have no pending PCM.

## Verified candidate

The native archive builds, EFI linking succeeds and `validate-binary.py`
reports no required unresolved symbols or hosted syscall/sysenter instructions.
The candidate EFI SHA-256 is
`7a4451f374eafb466f09e507c0a21f3b1b8446cc44ba77debd645b769f73927e`.
`media-config-after-suite.json` retains three successful QEMU/KVM cases with
four vCPUs, CPL3 execution and no unexpected renderer fault:

| Case | Duration | Evidence |
| --- | ---: | --- |
| `mse-config` | 40.08 s | 7.9 s of each expected tone; 800 production packets; old configuration drains 400 packets before replacement. |
| `mse-queue` | 31.50 s | Timed PCM and fresh decoder output during a six-second JavaScript block; pause, resume and seek. |
| `mse-audio` | 43.80 s | 1100 production packets; VP8/Opus playback, seek, mono/stereo device output, mute and gain. |

Each case also completes 65 broker checks, 32 synchronous media checks and the
queue probe. Queue-probe assertion totals are 279, 279 and 280; polling makes
these counts variable. The completed configuration page was visually inspected.
The default EFI image remains unchanged.

`media-config-recovery-suite.json` subsequently passes forced renderer
termination and recovery in 47.64 seconds. Teardown releases 221493 bytes of
queued compressed data, and the replacement renderer passes the blocking-script
continuity test. This negative test intentionally stops the original renderer;
it is not included in the fault-free claim above. Both generations complete the
broker and queue boundary probes.

## Reproduction commands

```sh
python3 tests/make-mse-config-fixtures.py
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/media-config \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix media-config-after --require-cpl3 --require-no-renderer-faults \
  --require-media-service --require-broker-probe --require-media-queue-probe \
  mse-config mse-queue mse-audio
```

The candidate requires rebuilding the native Blink archive and the EFI image
with `ENABLE_MEDIA_SERVICE=1`, `ENABLE_BROKER_PROBE=1`, `ENABLE_BLINK=1`,
`BLINK_SOURCE_POSITIONS=eager` and `BLINK_TASK_DIAGNOSTICS=1`. The fixture server
must use the matching HTTPS certificate. The test checks an actual channel
configuration change and complete PCM content; it does not establish all MSE
configuration transitions, physical sound output or audiovisual synchronization.

## Live YouTube follow-up

`media-config-youtube-suite.json` records a successful 700.55-second run of the
same candidate on the public Big Buck Bunny YouTube page. The renderer remains
at CPL3 with no unexpected fault and the media service decodes 29,000 packets.
All twelve playback checks pass, including pause, resume, seek and a continuous
visible interval. In that interval 20.0116 seconds of wall time correspond to
20.01754 seconds of media time and 612 newly decoded frames. The measurement
listener is acknowledged while the video tab is hidden before the harness
activates the tab. This removes an installation race in the earlier harness;
it does not prove that race caused the earlier missing measurement.

The visible screenshot was inspected and shows video and the surrounding
YouTube page. The raw AC97 capture and an unchanged-PCM playable WAV are retained
as `media-config-youtube-raw.wav` and `media-config-youtube-playable.wav`, with
signal statistics in `media-config-youtube-audio.json`. These checks do not
measure audiovisual synchronization, physical speaker output or displayed-frame
cadence. Some sidebar glyphs remain missing in the captured page.
