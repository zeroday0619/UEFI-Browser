# MSE Opus and synchronized video

The subsequent [zero-rate correction](MSE-ZERO-RATE.md) stops DMA consumption
while `playbackRate` is zero and verifies resume without a media-clock jump.

The firmware player now uses Chromium's `OpusAudioDecoder` and existing libopus to decode clear mono/stereo Opus from MediaSource. It supports audio-only WebM, multiplexed VP8/VP9 and Opus, and separate audio/video SourceBuffers. Video presentation follows the AC’97 consumed-sample position. This fixture validation does not establish live YouTube playback.

## Implementation and ownership

`src/blink/media_source_audio.inc` manages decoder initialization, asynchronous reads, generation-scoped callbacks, seek reset, pre-skip/discard padding through the upstream decoder, PCM conversion and bounded refill. It retains at most one decoded Opus packet and uploads at most 4096 stereo frames per call. The player limits advance decoding to approximately half a second. Timestamp gaps use bounded silence uploads; seek trimming and overlap removal preserve the shared presentation clock.

`src/audio/ac97-stream.h` owns the streaming DMA queue on the BSP. A clip has 32 descriptor slots with at most 31 queued, 16 KiB per PCM slot, and separate source samples for volume changes. Submission offsets must match the cumulative frame count. Pause rebases pending descriptors after retaining the partially consumed samples. Underflow and subsequent LVI updates resume queued data. Device position uses CIV/PICB and counts only complete stereo frames. Renderer code supplies copied PCM, never descriptor addresses or DMA storage.

The broker now validates streaming uploads with the same bounded copy as finite PCM uploads. Negative tests exposed a separate allocation-boundary bug: a rejected byte-range check fell through to a wider page grant. Exact allocation bounds now constrain that fallback. Page grants, private image aliases, stack, TLS and shared immutable segments also require current renderer page permissions before privileged access. Read-only and inaccessible pages cannot be written through the broker. These checks strengthen existing boundaries; they do not constitute a complete sandbox audit.

The capability gate parses standard VP9 codec strings through Chromium's original parser. Eight-bit profile 0 is supported; invalid profiles and ten-bit video remain rejected. Ordinary URL WebM playback still supports video-only clips. URL Opus, compressed Web Audio and other compressed-audio paths remain incomplete.

## Validation

The final media release image has SHA-256 `4f70d35d6f802d50053860c678fee8b930553f5ca4f86073f7fe4dc3403eb594`; its payload hash is `041f033dbd63c24101a2098c527466b5183bcb8d9625b49e2fb31d72def71f70`. The broker-probe image has SHA-256 `8850f4df47f7a0138a76aba25f0d354a35b0be92721940173d7484c6fd37ffb5`.

- Probe suite (`mse-audio-final-suite.json`; local artifact): MSE audio/video, original MSE video, MSE boundaries, CJK input and DevTools all passed. Every scenario executed at CPL3 and completed 65 broker checks without an unexpected renderer fault.
- Release suite (`mse-audio-final-release-suite.json`; local artifact): MSE audio/video, existing PCM output, URL video and audio limits all passed at CPL3 without unexpected renderer faults.
- Static binary validation (`mse-audio-final-binary.log`; local artifact): EFI and ELF load bounds, required symbols and absence of hosted syscall/sysenter instructions passed.
- DMA host test (`mse-audio-final-dma-host.log`; local artifact): production queue code passed AddressSanitizer and UndefinedBehaviorSanitizer checks for capacity, sample ordering across 300 wraps, partial pause/resume, underrun/refill and reset.

The 17 MSE audio assertions cover capability parsing, multiplexed and separate streams, visible red/green/blue frame changes, the device clock, pause/resume, seek, mute/volume, end-of-stream, buffering exhaustion and appended-data recovery, and audio-only attachment. Green/blue frame pixels are checked through QEMU screendumps; the release blue screenshot was also visually inspected.

Captured QEMU AC’97 output is analyzed as PCM, independently of DOM time. The probe capture contains the expected changing stereo tone pairs, attenuation and silence, with 20 consecutive 100 ms windows of the first tone. The checker preserves the raw capture and writes a normalized WAV header without modifying the PCM. These results establish emulator audio output, not physical-speaker playback. See `mse-audio-final-mse-audio-spectrum.json` and `mse-audio-final-release-mse-audio-spectrum.json`.

```sh
python3 tests/audio-stream-host.py
BUILD_DIRECTORY="$PWD/build/mse-audio" python3 validate-binary.py
python3 tests/network-servers.py --directory build/mse-audio-final-https
# Run the following command in another terminal while the server is running.
python3 tests/state-suite.py --cpus 4 --workers 2 \
  --build-directory build/mse-audio \
  --https-ca build/mse-audio-final-https/ca.esl \
  --prefix mse-audio-final-release --require-cpl3 \
  --require-no-renderer-faults mse-audio audio media audio-limits
```

Earlier failing runs remain in validation. They exposed invalid muxed fixture timestamp ordering, the allocation-boundary fallback, an EOS pause that replaced final presentation time with shorter decoded audio time, and an audio-only `SizeChanged` callback that violated Blink's video invariant. The fixtures and production code were corrected before the passing suites; Chromium assertions remain enabled.

## Remaining work

Decoding and refill run on the renderer sequence. Long synchronous JavaScript can exhaust the approximately half-second audio queue and delay video presentation. Independent media scheduling, broad MSE conformance, mixing, general resampling, pitch preservation, additional codecs, encrypted media, hardware decoding and physical audio-device validation remain incomplete. The subsequent [live YouTube investigation](YOUTUBE.md) reached visible video and PCM output but then a player error. Seeking and sustained playback remain unverified.

The subsequent authenticator availability correction was validated together with MSE audio in `youtube-auth-suite.json` and promoted to the default image. See [the live investigation](YOUTUBE.md) for the newer image hash and remaining failure.
