# Bounded MSE packet batching

## Runtime boundary

The preceding [callback dispatch change](AUDIO-DISPATCH.md) removed redundant
queue transitions, but every compressed packet still required another posted
demuxer completion. With competing renderer tasks, a 20 ms Opus packet could
take much longer than 20 ms to replenish the audio device.

`src/musl/threads.c` still implements cooperative contexts. Creating another
Chromium thread alone would not provide independent decoding during an
unyielding JavaScript task. This change addresses packet throughput on the
existing sequence; preemptive or CPU-parallel media execution remains open.

## Implementation

`media_source_player.inc` and `media_source_audio.inc` request up to eight
compressed packets per demuxer read. The pinned `ChunkDemuxerStream` supports
partial batches and stops a batch before a configuration transition. The
adapter rejects empty/oversized batches, null entries and EOS before the last
entry. Each stream retains at most one batch and one outstanding read.

Each processing task checks a four-millisecond deadline between packets.
Individual codec calls cannot be preempted by this budget. At expiration, one
coalesced continuation resumes the batch. Every packet still observes PCM
backpressure, the existing approximately half-second look-ahead and the 64 MiB
decoded-video limit. The batch does not bypass those checks.

Seeking and source teardown discard queued packets and reset continuation state.
Continuations carry the media generation so stale work cannot clear a newer
generation's scheduling flag. Weak references and generation checks protect the
processing loops across Blink notifications. Presentation, seek completion,
readiness notifications and timer/progress paths also stop when a notification
destroys or replaces the player. Codec and DMA/broker ownership stay unchanged.

## Build and local validation

The candidate is `build/media-batch/esp/EFI/BOOT/BOOTX64.EFI`, SHA-256
`cb2f0dab404bc1be927afd56c4c80318a4186a0e547066af0b76f77ef985dd78`.
`media-batch-sources.json` records source and image identities. Native compilation,
EFI linking and `validate-binary.py` passed. The default image is unchanged.

```sh
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/media-batch" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/media-batch" python3 validate-binary.py
```

The three changed media source files were copied into the prepared Chromium
checkout's `third_party/blink/uefi/src/blink` directory before native compilation.

`media-batch-after-suite.json` records six passing QEMU 11.1.2/KVM scenarios with
four virtual CPUs: `mse-batch`, `mse-dispatch`, `mse-audio`, `mse-rate`,
`media-source` and `media-source-boundaries`. All ran at CPL3 without unexpected
renderer faults and required successful firmware watchdog cancellation.
The scenarios cover actual Opus AC'97 output, pause/zero-rate/resume, seeks,
pending-seek cancellation, VP8/VP9 configuration transitions, append windows,
source removal/reattachment, EOS and cumulative video decoding beyond 64 MiB
while retaining the bounded frame queue. Logs confirm eight-packet reads for
both video and audio. These checks do not establish full media conformance.

## Controlled queue-pressure comparison

The `mse-batch` scenario loads local HTTPS Opus data and uses four MessageChannels.
Each has at most one posted message, runs 20 ms of JavaScript, then reposts
itself. Concurrent animation callbacks run 40 ms of JavaScript and change the
page background. This creates a bounded, continuously runnable workload without
one unbounded JavaScript task. Media advancement is compared with actual
`performance.now()` time over a nominal four-second interval.

| Measurement | Previous dispatch image | Batched candidate |
| --- | --- | --- |
| Image prefix | `87e39554` | `cb2f0dab` |
| Elapsed seconds | 4.0888 | 4.1001 |
| Media seconds advanced | 1.3722 | 4.0979 |
| Media/elapsed ratio | 0.3356 | 0.9995 |
| Message callbacks | 183 | 182 |
| Animation callbacks | 8 | 8 |
| 0.95–1.05 playback gate | Failed | Passed |

`media-batch-baseline-suite.json` retains the expected previous-image failure.
`media-batch-comparison.json` contains both measurements and image identities.
This is one sequential A/B pair and does not establish independent media
execution during arbitrary long tasks. Frame presentation was still delayed by
the deliberately saturated renderer; improved media progression is not a claim
that all UI work met its deadline.

The earlier `media-batch-before` experiment used only 120 ms animation callbacks
and did not reproduce the throughput problem. The first queued-work experiment
(`media-queue-before`) produced the slowdown but also hit an overly strict
animation-count condition. The final baseline above corrected that condition
and failed specifically on the playback ratio. Those earlier records are
preserved separately.

With `tests/network-servers.py --directory build/media-batch-https` running:

```sh
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/media-batch \
  --https-ca build/media-batch-https/ca.esl --prefix media-batch-rerun \
  --require-cpl3 --require-no-renderer-faults \
  mse-batch mse-dispatch mse-audio mse-rate media-source media-source-boundaries
```

Use `build/audio-dispatch` and a fresh prefix for the baseline. To launch the
candidate interactively from the project directory:

```sh
BUILD_DIRECTORY="$PWD/build/media-batch" QEMU_MEMORY=4096 ./run-qemu.sh
```

## Live YouTube validation

The first `media-batch-youtube` run ended after 242.36 seconds because the
Console prompt did not appear within its 30-second readiness limit. The
renderer remained alive. The retry used the same EFI image and a fresh disk
with `YOUTUBE_CONSOLE_TIMEOUT=120`. The prompt appeared after 31.87 seconds.
The longer readiness allowance did not change the playback acceptance bounds.

`media-batch-youtube-retry-run.json` records the completed 497.66-second run.
The video was visible at 180 seconds. Console-mediated pause held the clock
at 213.315 seconds, resume advanced it, and seeking to 120 seconds completed.
No player media error or unexpected renderer fault was recorded. The run still
failed both real-time progression checks:

| Interval | Page clock seconds | Media seconds | Ratio |
| --- | ---: | ---: | ---: |
| Initial playback | 21.3744 | 16.0701 | 0.7518 |
| Resumed playback | 21.5599 | 15.4114 | 0.7148 |
| After seeking | 16.9468 | 10.9403 | 0.6456 |

The player tab was visible during waits and the inspector was visible during
commands. Before opening the inspector, status samples covering 102.418 seconds
of firmware time showed 95.214 seconds of media advancement (0.9297 ratio).
This observational live-service run is not a controlled comparison against
earlier network responses. It does not prove uninterrupted playback, pointer
control behavior or audiovisual synchronization.

The `media-batch-youtube-retry-*` artifacts retain timing, control samples,
Console readiness, screenshots, serial output, reset logs and captured-audio
statistics. `media-batch-retry-sources.json` records the source and image
identities. The default `build/blink` image was not changed.
