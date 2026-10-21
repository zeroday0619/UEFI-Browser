# Renderer memory pressure

The live site previously reached the renderer's 2 GiB physical data quota after [page-table growth](PAGE-RECLAMATION.md) removed an independent mapping limit. The embedder did not construct Chromium's memory-pressure listener registry or send pressure notifications to Blink and V8.

## Implementation

`renderer_runtime_memory_statistic()` exposes three scalar values through broker operation 14: charged physical bytes, the data quota and allocation count. The BSP reads its own accounting. It rejects unknown fields and nonzero unused arguments; no output pointer, address or caller-selected allocation size crosses this interface. The physical quota is unchanged.

`browser_entry.cc` constructs the listener registry before Blink initialization. `remote_frame.cc` samples physical and tracked C heap usage at most once every 250 ms between renderer tasks. Either budget at 65% raises moderate pressure; 80% raises critical pressure. It notifies the registry and all Blink-managed main-thread and worker isolates. Critical pressure also evicts the Blink resource cache and purges unused Skia caches. Repeated notifications and de-escalation have a five-second minimum interval; escalation is immediate. A guard prevents nested event loops from recursively triggering collection.

Logs record the pressure level, physical quota, physical and C heap usage before and after notification, and elapsed notification time. This polling cannot interrupt an individual JavaScript task that exhausts memory before returning to the event loop. A critical collection can also pause the renderer for seconds. Neither limitation is resolved by the current change.

## Evidence

The normal EFI image is `3905e9fd9aadfa27f2d47edf545c19dc90b046ca6f8f22e008ce3e3da8eb3ca9`, with payload `a8c0f734b41a4b680bccd523266aa90708ddee5d1088e56834a4fd18d998226d`. The probe image is `ca629c572e6d901e02977ceec6289a94ba465f95e1a29439e788b74316cc0664`. These are separate test images; the default image remains unchanged.

The first live YouTube run reached its 180-second observation without renderer termination. Critical notification reduced charged physical backing from 1,726,128,128 to 1,024,262,144 bytes and C heap usage from 650,186,740 to 96,310,865 bytes. The operation took 3,882,077 microseconds. The 180-second screenshot shows a video frame and the live page UI. The subsequent DevTools Console prompt did not appear within 30 seconds, so the overall observer failed. Frontend loading alone is not a successful inspector test.

The raw AC'97 capture from that run contains 194.40 seconds of stereo PCM, including 117 one-second windows with RMS above two. Its RIFF data length was not finalized. These counts establish captured sound, not uninterrupted or synchronized playback. `memory-pressure-youtube-audio.json` retains the measurement. The QEMU runner closes its original QMP stream before opening the shutdown connection. The subsequent `audio-shutdown` MSE-audio regression passed in 44.24 seconds, but `audio-shutdown-mse-audio-spectrum.json` still records `raw_header_finalized: false`. Closing the stream did not resolve WAV finalization. The separate playable WAV preserves the captured PCM; normalization is not evidence of a finalized raw container.

`memory-pressure-probe-suite.json` passes all 14 sandbox boundary cases and live fault recovery, plus repeated 64 MiB reclamation and 2,304 sparse mappings. The memory probe also checks the new statistics values and malformed broker requests. `memory-pressure-normal-suite.json` records the normal DevTools, asynchronous-scroll, worker, WebGL, WebGPU and MSE-audio regressions. Native compilation, EFI linking and static binary validation passed.

A separate 360-second observation in `build/memory-pressure-long` recorded samples through 270 seconds without renderer termination, then QEMU exited with status zero before the next sample. The runner reported failure. The retained log does not establish the emulator exit cause, and this run does not prove six-minute playback.

## Reproduction

```sh
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/memory-pressure" ./build-musl.sh
ENABLE_BLINK=1 ENABLE_MEMORY_PROBE=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/memory-pressure-probe" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/memory-pressure" python3 validate-binary.py
```

With the fixture server running using `--directory build/memory-pressure-https`:

```sh
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --https-ca build/memory-pressure-https/ca.esl \
  --build-directory build/memory-pressure-probe --prefix memory-pressure-probe \
  --require-cpl3-probe --require-memory-probe sandbox
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --https-ca build/memory-pressure-https/ca.esl \
  --build-directory build/memory-pressure --prefix memory-pressure-normal \
  --require-cpl3 --require-no-renderer-faults \
  devtools renderer-scroll workers webgl webgpu mse-audio
```

Use a fresh prefix for another raw-disk suite. The YouTube observer accepts `YOUTUBE_OBSERVE_SECONDS` in 30-second increments from 180 to 1,800. Observations remain diagnostics and require visual, audio and interaction checks before claiming playback support.
