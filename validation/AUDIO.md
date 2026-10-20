# PCM audio output validation

The GPU availability statements below describe this report's recorded build. See [software GPU validation](SOFTWARE-GPU.md) for the subsequent ANGLE, Dawn and SwiftShader implementation.

This report describes the audio milestone. [Worker execution validation](WORKERS.md) supersedes its dedicated-worker availability result.

## Implemented path

The Blink media player decodes bounded PCM WAV data and uploads stereo signed 16-bit samples to a BSP-owned audio service. `src/audio/ac97.c` discovers the QEMU Intel AC’97 PCI function, allocates DMA memory below 4 GiB, programs the descriptor list and starts the PCM output engine. The device consumes samples independently of the renderer's JavaScript task. The driver uses the AC’97 register interface described by the [QEMU device implementation](https://github.com/qemu/qemu/blob/master/hw/audio/ac97.c); no QEMU source is incorporated into the firmware.

`runtime/firmware_audio.h` defines create, upload, start, stop, position, volume and close operations. AP-renderer uploads use a bounded stack copy through the existing runtime channel. The BSP validates handle ownership, upload order, sizes, rate and resource limits. These checks prevent API misuse but do not establish a hostile-renderer security boundary.

Playback position is derived from the hardware descriptor index and remaining sample count. Pause saves this position and stops DMA. Seek rebuilds descriptors at the requested frame. End and replay are connected to Blink media events. Volume and mute scale an immutable PCM source into the DMA buffer using linear gain. This avoids dependence on the emulator's mixer attenuation curve. Mono input is expanded to two channels, and unsigned eight-bit input is converted to signed sixteen-bit output.

Bus mastering is disabled before replacing or freeing active DMA buffers. Close, renderer reclamation and final firmware teardown release the clips. Failed page allocation leaves a zero address, so teardown cannot free an allocation sentinel.

## Supported scope

- HTTP(S) PCM WAV format tag 1, eight- or sixteen-bit samples, mono or stereo, 8–48 kHz.
- Eight loaded clips across the service and one active output at a time. A conflicting active clip receives a media error. Mixing is not implemented.
- At most 4,194,176 converted PCM bytes per clip, plus an equal-sized source copy and temporary decode/upload storage. The existing encoded-response limit remains 8 MiB.
- Complete buffered clips only. Compressed audio, WebM audio tracks, audio/video synchronization, streaming, MSE, Web Audio, capture, device selection and pitch-preserving rate changes are not implemented.
- Rate changes use the DAC frequency and are limited by its 8–48 kHz range. They change pitch. General playback-rate conformance has not been verified.
- The driver recognizes Intel 8086:2415 on PCI bus 0. QEMU is verified; other devices and physical hardware are not.

The renderer still runs at CPL0. Dedicated/shared/service-worker execution, WebGL/WebGPU context providers and a renderer security sandbox remain unimplemented. This audio path does not supply those services.

## Verification

Tested image SHA-256: `9e2e22ff762e560ac0322953db3864123d0c9ee2e8748012f99d9481dde921db`. The runtime uses Chromium 158.0.8110.0 and musl 1.2.5. QEMU 11.1.2 runs with OVMF, KVM and 4 GiB RAM. Four vCPUs exercise the AP renderer; one vCPU exercises the local renderer.

`audio` has 15 page assertions covering load, duration, device progress, progress across a 1.2-second synchronous JavaScript task, pause, seek, mute/quiet progress, end, replay, reload, and mono eight-/sixteen-bit playback. The host separately analyzes samples captured from the emulated device. It requires the 440/660 Hz stereo pair, the 880/1100 Hz pair after seeking, muted windows between the loud and quiet segments, quarter-amplitude output, and mono signals at 22.05 and 16 kHz. A DOM timer alone cannot satisfy this test.

The first test incorrectly compared `currentTime` twice within one JavaScript task. Blink intentionally keeps that value stable until a microtask boundary. The corrected test reads after yielding and independently verifies the captured output. The first amplitude test also detected that hardware mixer attenuation did not produce the requested linear gain in QEMU. Software PCM gain corrected the output; the test threshold was not weakened.

`audio-limits` has 11 assertions covering eight loaded clips, rejection of a ninth clip, malformed input, cleanup and successful reloading. The WebM playback and failure/recovery tests remain in the suite, together with IndexedDB, Cache Storage, service probes, asynchronous input and tab/site interaction.

The final image passed all nine four-vCPU scenarios and all three one-vCPU scenarios. Both audio runs passed the 15 playback assertions, 11 resource-limit assertions and independent waveform checks. The four-vCPU capture contains 21 first-tone windows, nine second-tone windows, six attenuated windows, five silent windows and nine windows for each mono format. Each analysis window spans 100 milliseconds. The binary validator found no required unresolved symbols or syscall instructions. Service-probe success means that the report was collected correctly; its unavailable worker and GPU results remain failures to provide those features.

Evidence:

- [Four-vCPU suite](audio-final-suite.json) and [one-vCPU suite](audio-final-single-suite.json).
- [Four-vCPU waveform measurements](audio-final-audio-spectrum.json) and [one-vCPU measurements](audio-final-single-audio-spectrum.json).
- [Raw capture](audio-final-audio-raw.wav) and [playable WAV](audio-final-audio-playable.wav).
- [Service results](audio-final-service-capabilities-service-capabilities-results.json), which distinguish working PCM/WebM/IndexedDB paths from unavailable workers and GPU providers.
- [Native build](audio-final-native-build.log), [EFI link](audio-final-link.log), [binary validation](audio-final-binary.log) and [static checks](audio-final-static-checks.json).

The installed QEMU WAV backend left zero RIFF/data size fields even after QMP quit. The raw capture is preserved unchanged. The analyzer writes a separate playable container with correct sizes and verifies that its PCM bytes exactly match the raw capture. Reports include the raw and PCM hashes and whether the original header was finalized. This is a container-header repair, not synthesized or substituted audio. The [upstream WAV backend](https://github.com/qemu/qemu/blob/master/audio/wavaudio.c) describes its output format. Capture verification does not establish playback through physical speakers.

## Reproduction

From the project directory, generate the deterministic inputs and run the fixture server in a separate terminal:

```sh
python3 tests/make-audio-fixtures.py
python3 tests/make-media-fixtures.py
python3 tests/network-servers.py
```

```sh
python3 build-blink-libraries.py --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 tests/state-suite.py --https-ca build/network-https/ca.esl --prefix audio-final audio audio-limits media media-boundaries indexeddb cache service-capabilities scroll-input tabs-site
python3 tests/state-suite.py --workers 1 --cpus 1 --https-ca build/network-https/ca.esl --prefix audio-final-single audio audio-limits media
```

The normal launcher adds AC’97 with a PipeWire backend. `QEMU_AUDIO_DRIVER=pa ./run-qemu.sh` selects PulseAudio; `QEMU_AUDIO_DRIVER=none ./run-qemu.sh` suppresses host sound. The automated tests use the WAV backend and do not require a host sound server. Stop the fixture server after testing.
