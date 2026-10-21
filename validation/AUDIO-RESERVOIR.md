# BSP-owned streaming audio reservoir

The firmware retains decoded PCM independently of the renderer event loop.
`src/audio/ac97-stream.h` separates 128 software slots from the 32 AC'97 DMA
descriptors. At most 31 descriptors are published at once so the hardware
current-index register cannot alias a complete ring rotation. The browser's
BSP input poll retires completed descriptors and publishes retained packets
while JavaScript blocks the renderer. Each stream allocates 2 MiB of DMA PCM
and 2 MiB of gain-source PCM. The existing eight-handle limit remains in force.

Renderer uploads still cross the existing owner-checked audio broker and are
copied into firmware storage. Descriptors never reference renderer memory.
Pause preserves partially consumed PCM, resume rotates metadata without
duplicating samples, and seek/reset discards the retained queue. Volume
changes apply to all retained packets. The MSE Opus look-ahead target increases
from 0.5 to 2 seconds; decoding still uses the existing bounded packet batches
and four-millisecond task budget.

This is finite buffering. Decoding and demuxing still execute in the renderer.
Short Opus packets can exhaust the slot count before reaching the two-second
target. A longer script, video underflow, network starvation or a blocked BSP
can still interrupt playback. The single-CPU fallback cannot service the BSP
poll while its JavaScript runs. Independent media execution and presentation
remain necessary for sustained playback under arbitrary renderer workloads.

## Build and host checks

The candidate image is `build/audio-reservoir/esp/EFI/BOOT/BOOTX64.EFI`, SHA-256
`4abfc0a59e1de06627884c2a4feae798c960a095532e661e6c650ffb6ae7d7da`.
It uses eager V8 source positions and scheduler diagnostics. Native compilation,
EFI linking and binary validation passed. The default `build/blink` image was
not replaced.

```sh
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
BLINK_SOURCE_POSITIONS=eager BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/audio-reservoir" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/audio-reservoir" python3 validate-binary.py
python3 tests/audio-stream-host.py
python3 tests/audio-output-observer-host.py
```

The host test compiles the production queue with AddressSanitizer and
UndefinedBehaviorSanitizer. It checks software/hardware wraparound against
exact stereo sample order, the 128-slot bound, partial pause/resume, delayed
hardware restart after underrun, invalid register observations, BSP-only
refill and reset. This is a register model, not physical-device validation.

## Controlled QEMU comparison

The local HTTPS fixture `tests/input-pages/media-reservoir.html` plays Opus,
blocks JavaScript for 1.5 seconds, then checks playback progress, pause and
resume. Both images run at CPL3 with four virtual CPUs and no unexpected
renderer fault. `audio-reservoir-comparison.json` retains exact values.

| Measurement | Prior image | Reservoir image |
| --- | ---: | ---: |
| Measured blocked interval | 1.5066 s | 1.5055 s |
| Media position advance | 0.515146 s | 1.509083 s |
| Media/wall-clock ratio | 0.341926 | 1.002380 |
| Position change while paused | 0 s | 0 s |
| Position advance after resume | 0.291813 s | 0.291688 s |
| Consecutive non-silent captured PCM | 1.8 s | 3.2 s |

The prior image is `build/eager-source-positions`, SHA-256
`91fa42ad0d631ba4c0bcb46a640035c3674f7192cbf44611abf487c754cc7785`.
The corrected prior run is `audio-reservoir-before-fixed`; its failure is the
negative control. The candidate `audio-reservoir-after` passes.

The earlier `audio-reservoir-before` run read `currentTime` twice in the same
JavaScript task and must not be used as a performance comparison. The fixture
now yields to a new task before reading the final position because the
[HTML official playback position](https://html.spec.whatwg.org/multipage/media.html#official-playback-position)
is stable while scripts execute.

WAV sample duration does not encode individual write times. The initial
`reservoir_continuous_seconds` report field therefore means consecutive
non-silent PCM duration, not proven wall-clock continuity. The verifier now
uses `reservoir_nonsilent_pcm_seconds` and explicitly records that timing is
unverified by the WAV alone. The
[QEMU WAV backend](https://raw.githubusercontent.com/qemu/qemu/v11.1.0/audio/wavaudio.c)
writes supplied PCM bytes without per-write timestamps.

`tests/audio-output-observer.py` additionally records file-size growth against
the host monotonic clock every 20 ms between serial markers surrounding the
blocked task. It excludes 100 ms at each edge and requires at least a one-second
interior, output rate within 0.9–1.1 of 192000 bytes/second, observer gaps at
most 80 ms and write gaps at most 150 ms. It rejects stalled output and delayed
bulk writes in synthetic checks. Each file offset is matched to captured PCM;
the same rate and gap limits apply to frames where either channel exceeds an
absolute sample value of 100. This additional signal gate rejects underrun
silence that the backend writes at the normal byte rate. This measures emulator output with bounded
sampling resolution, not physical speaker latency or audiovisual synchronization.

Start or reuse the fixture server with
`python3 tests/network-servers.py --directory build/inspector-timing-https`, then:

```sh
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/audio-reservoir \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix audio-reservoir-observed-after \
  --require-cpl3 --require-no-renderer-faults mse-reservoir
```

Use a fresh prefix for each run. Select `build/eager-source-positions` to
reproduce the negative control without rebuilding either image.

The candidate `audio-reservoir-observed-after` passed in 29.17 seconds at CPL3
without a renderer fault. Its interior observation covered 1.318280 seconds
and 253952 PCM bytes, giving an output/wall-clock ratio of 1.003328. Maximum
sample spacing was 20.41 ms and the maximum observed write gap was 40.69 ms.
The `audio-output-observation-results.json` artifact includes every sample.
The corresponding prior-image run failed in 26.81 seconds. Both files grew
at the nominal byte rate, but the prior capture contains 0.9 seconds of silence.
The `timed-pcm-results.json` artifacts preserve the original observation and
capture hashes and match PCM frames to those recorded host-time offsets.
The prior signal/wall-clock ratio is 0.325244 with an 872.07 ms observed signal
gap; the candidate ratio is 1.001352 with a 40.69 ms gap. The original
file-growth-only observer could not distinguish this silence and has been
replaced by the combined byte/signal checks.

The final integrated candidate run `audio-reservoir-timed-pcm-after` passes
all byte and signal gates in 28.14 seconds at CPL3 without a renderer fault.
Its measured interior is 1.297018 seconds, signal/wall-clock ratio 1.001351,
maximum sample spacing 20.39 ms and maximum observed signal gap 40.56 ms.
The final integrated prior-image run `audio-reservoir-timed-pcm-before` fails
the signal-rate and signal-gap gates in 25.72 seconds, with a 0.325502 ratio
and an 871.75 ms signal gap. It remains at CPL3 without a renderer fault.

## Regression results

`audio-reservoir-regression-suite.json` records passing `audio` (32.17 s),
`mse-audio` (42.64 s), `mse-rate` (25.93 s), `mse-batch` (36.36 s),
`media-source-boundaries` (33.19 s) and `audio-limits` (23.08 s). All six run
at CPL3 without unexpected renderer faults. WAV analysis also passes the
existing stereo tone, seek, mute and gain checks for their audio fixtures.

```sh
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/audio-reservoir \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix audio-reservoir-regression \
  --require-cpl3 --require-no-renderer-faults \
  audio mse-audio mse-rate mse-batch media-source-boundaries audio-limits
```

## Live YouTube result and remaining work

`audio-reservoir-youtube-suite.json` completes in 671.35 seconds at CPL3 with
no unexpected renderer fault. It correctly exits 1: initial playback advances
at 0.891799 of wall time, below the unchanged 0.9 acceptance threshold.
Pause holds; resume advances at 0.900388 and post-seek progression at 0.915742.
Seeking completes without a media error. The independent visible-video
interval lasts 20.0106 seconds and advances 18.447062 seconds, a ratio of
0.921865, with 556 decoded frames and no visibility transition, pause, seek
or media error. The live screenshot shows Big Buck Bunny in the browser.

```sh
YOUTUBE_LONG=1 YOUTUBE_INSPECT=1 YOUTUBE_PLAYBACK_PROBE=1 \
  YOUTUBE_CONSOLE_TIMEOUT=120 QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 \
  python3 tests/state-suite.py --cpus 4 --workers 1 \
  --build-directory build/audio-reservoir \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix audio-reservoir-youtube \
  --require-cpl3 --require-no-renderer-faults youtube-observe
```

This is one live run, with variable site scripts and network conditions.
Separate UI and fixture VMs ran during earlier portions of the session, but
had exited before the independent visible-video interval. It is not an
isolated whole-run benchmark against the earlier image. The visible ratio
passes the present tolerance but still falls short of exact wall-clock speed.
Captured stereo PCM and decoded video do not establish audiovisual sync or
physical output quality.
The retained WAV contains 554.9825 seconds of stereo PCM with per-channel RMS
2381.44 and 2474.25. `audio-reservoir-youtube-youtube-observe-audio-summary.json`
records format, raw/PCM hashes and the unverified physical-output/sync flags.

`audio-reservoir-youtube-stalls.json` retains the largest scheduler callback
records and media reports. Renderer callbacks lasting longer than the
two-second audio target remain, and video look-ahead is still approximately
half a second. `TickMse` pauses audio on video underflow, so a larger PCM queue
alone cannot eliminate the remaining stalls. The next work is independent
media decoding/presentation with safe lifecycle and seek cancellation, plus
tracing of long callbacks. Full YouTube support and the broader nine-goal
objective remain incomplete.
