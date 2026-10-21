# MSE audio callback dispatch

## Queue overhead

The pinned `ChunkDemuxerStream::Read` implementation already posts its completion
to the calling sequence. The UEFI adapter wrapped that callback in a second
posted task. Opus decoding itself ran synchronously, but the decoder's default
asynchronous mode posted PCM output and completion as two more tasks. Continuous
rendering could interleave with each queue transition before the next compressed
packet was requested.

`media_source_audio.inc` now selects Chromium's synchronous Opus mode. Its output
callback only retains one bounded PCM result, and its completion callback only
stores the decode status. PCM submission and Blink notifications run after
`Decode` returns. A weak reference and the existing generation check guard
subsequent completion against reentrant destruction, seeking or source changes.
Initialization remains posted so metadata setup finishes before readiness
notifications. An unexpected second output is rejected instead of accumulating
an unbounded queue.

Both audio and video stream reads now use the callback posting already supplied
by `ChunkDemuxerStream`. Other demuxer host callbacks retain their posting boundary.
Audio therefore uses one queued read completion per packet instead of a chain
of four queued callbacks. Decoder, PCM validation, DMA ownership and CPL3 broker
boundaries remain in place.

This change keeps decoding on the renderer main sequence. It does not provide
independent media execution during arbitrary long JavaScript tasks or guarantee
continuous live-site playback.

## Build and regression evidence

The isolated candidate is `build/audio-dispatch/esp/EFI/BOOT/BOOTX64.EFI`, SHA-256
`87e39554740343dde9230962c79eb659c0e3b00fc8b54cacbe5f16fd6c1a8494`.
`audio-dispatch-sources.json` records source, pinned upstream contract and image
hashes. Native media-player compilation, browser archive construction, EFI
linking and `validate-binary.py` passed. The default image was not replaced.

```sh
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/audio-dispatch" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/audio-dispatch" python3 validate-binary.py
```

The changed `src/blink/media_source_audio.inc` and `media_source_player.inc` were
copied to the corresponding `third_party/blink/uefi/src/blink` directory in the
prepared Chromium checkout before native compilation.

`audio-dispatch-after-suite.json` records five passing scenarios: `mse-dispatch`,
`mse-audio`, `mse-rate`, `media-source` and `media-source-boundaries`. All ran at
CPL3 without unexpected renderer faults and required the successful firmware
watchdog cancellation marker. The MSE audio scenario passed its AC'97 stereo,
mute, attenuation and continuity checks. These checks cover local selected
codecs and controls, not complete media conformance or physical speaker output.

## Controlled rendering workload

The `media-dispatch.html` fixture measures three intervals after loading
Opus data over local HTTPS: idle playback, a 40 ms JavaScript task every 80 ms,
and a 40 ms animation callback that changes the page background every frame.
It seeks between stages, logs actual elapsed time and media advancement, and
checks that the load executed. The last stage was added after the initial
`audio-dispatch-before`/`audio-dispatch-after` interval-only runs showed no
slowdown. Those earlier records are preserved separately.

The three-phase fixture was run sequentially against the unchanged watchdog image and
the candidate with fresh disks, four virtual CPUs and QEMU/KVM. Both performed
50 periodic tasks and 78 animation callbacks, totaling 2.000 and 3.120 seconds
of JavaScript work respectively.

| Phase | Previous image: media/elapsed time | Candidate: media/elapsed time |
| --- | --- | --- |
| Idle | 0.9935 | 0.9938 |
| Periodic JavaScript | 0.9991 | 0.9991 |
| Continuous animation and painting | 0.8786 | 1.0008 |

The painted interval advanced 3.5522 media seconds in 4.0429 seconds before the
change and 4.0070 media seconds in 4.0037 seconds afterward. This is one A/B pair,
not a general browser throughput estimate. Both initial runs passed the then-broad
0.85–1.15 acceptance interval; the recorded measurements expose the improvement.

The final regression gate narrows acceptance to **0.95–1.05** and repeats the
same workload. `audio-dispatch-gate-before-suite.json` records the expected
baseline failure: 3.5423 media seconds in 4.0023 seconds, ratio 0.8851.
`audio-dispatch-gate-after-suite.json` passes: 4.0184 media seconds in 4.0098
seconds, ratio 1.0021. Both again executed 78 animation callbacks and 50 periodic
tasks at CPL3 without unexpected renderer faults. `audio-dispatch-comparison.json`
retains both A/B pairs. `audio-dispatch-gate-sources.json` records the final test
source; the earlier manifest preserves the initial, looser fixture identity.

With `tests/network-servers.py --directory build/watchdog-media-https` running:

```sh
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/audio-dispatch \
  --https-ca build/watchdog-media-https/ca.esl --prefix audio-dispatch-rerun \
  --require-cpl3 --require-no-renderer-faults \
  mse-dispatch mse-audio mse-rate media-source media-source-boundaries
```

Use `build/media-timing-watchdog` and a separate prefix for the baseline.

## Live YouTube control run

`audio-dispatch-youtube-run.json` records a 485.59-second QEMU/KVM run with a
fresh disk, four virtual CPUs, 4 GiB RAM, CPU-reset logging, CPL3 enforcement,
watchdog-cancellation enforcement and AC'97 capture. It used the candidate image
above and left the default image unchanged. The harness completed every sample
and then returned **exit code 1** because the real-time playback checks failed.
This is a completed diagnostic run, not a passing overall playback benchmark.

The actual Console received commands through QEMU keyboard events. The video
tab was selected during waits and the inspector during commands. All returned
media errors were null. Pause, resume, seek completion and advancement after
seek passed, while the two real-time checks failed.

| Measurement | Observed result |
| --- | --- |
| Initial control interval | 11.2899 media seconds in 21.2249 page-clock seconds; ratio 0.532. |
| Pause | Position stayed at 150.5435 seconds for 11.7617 seconds. |
| Resume | 13.4000 media seconds in 21.0440 page-clock seconds; ratio 0.637. |
| Seek to 120 seconds | `seeking` became true, then false at 121.0745 seconds. |
| Progress after seek | Position reached 130.0046 seconds with no media error. |

The earlier watchdog image measured 0.196 and 0.323 for the corresponding two
control intervals. Those are separate live-service runs, so the comparison is
observational rather than a controlled network benchmark.

Before the inspector opened, samples within guest uptime 90–180 seconds advanced
78.537 media seconds over 85.806 clock seconds, a ratio of 0.915. The earlier
watchdog run advanced 41.420 over 84.231 seconds, a ratio of 0.492. None of the
18 candidate audio-lead samples in that window was at or below 20 ms, versus
16 of 17 earlier samples. These sampled leads do not establish uninterrupted
output between samples. Longer renderer stalls and playback below real time
remain, particularly while the inspector is active. The samples and comparison
scope are retained in `audio-dispatch-live-timing.json`.

`audio-dispatch-youtube-audio.json` records 18,759,484 stereo PCM frames at
48 kHz, including 16,897,881 nonzero samples, from the QEMU audio device capture.
The raw file and its hash are retained. This confirms decoded audio reached the
device output; it does not establish physical speaker output or A/V sync.

The [120-second video capture](audio-dispatch-youtube-input-youtube-120.png),
[post-seek video capture](audio-dispatch-youtube-input-youtube-playback-visible.png)
and [Console capture](audio-dispatch-youtube-input-youtube-playback-final.png)
were visually inspected. The final Console screenshot shows returned samples
through identifier 9 and the typed final command; serial output and the JSON
record contain the final identifier 15 response. These checks do not establish
physical A/V synchronization, pointer hit testing of YouTube controls, all
site features or hardware decoding. The subsequent
[bounded packet batching change](MEDIA-BATCHING.md) addresses queued-work
throughput. Media execution independent of long renderer tasks remains open.
