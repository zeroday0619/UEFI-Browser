# WebM video playback validation

This report records the video milestone and its tested image. The subsequent [PCM audio implementation and validation](AUDIO.md) supersede its audio-unavailable status.

## Implemented path

`src/blink/media_player.cc` supplies a real Blink `WebMediaPlayer` to main frames and local child frames. HTTP(S) media requests use the existing associated resource loader. Chromium's WebM parser extracts compressed packets, libvpx decodes VP8/VP9, and libyuv converts eight-bit I420 frames to CPU-backed ARGB `VideoFrame` objects. Blink paints those frames into the software display list and the existing Skia/GOP presentation path. Canvas `drawImage(video)` reads the same decoded frames.

The player implements play, pause, seek, positive playback rates, repeat playback, end-of-stream notifications, decoded-frame statistics and `requestVideoFrameCallback`. Parsing yields between bounded parser calls. Decoding yields between compressed packets on the renderer task sequence. A single parser or decoder call still runs to completion. Long synchronous JavaScript can delay video updates.

The implementation buffers and decodes the complete clip before reporting playback readiness. Its limits are 8 MiB of encoded data, 8,192 compressed packets, 64 MiB of retained frame allocations per player, 1920×1080 dimensions and one hour of declared duration. Decoder scratch space, packet metadata and paint caches are additional allocations. There is no global quota across players.

Only video-only, unencrypted, eight-bit VP8/VP9 WebM is supported. Audio tracks, audio-device output, MP4/H.264, MSE, DRM, streams, GPU decoding, full color management and opaque cross-origin media remain unsupported. Default media loads require the same origin. Explicit CORS requests use the resource loader's CORS checks. The tests cover denied cross-origin requests; a permitted cross-origin media response has not been verified. `canPlayType` advertises `video/webm` and the legacy VP8/VP9 codec strings within this scope. Modern extended codec strings are not advertised.

## Defects reproduced and corrected

1. A native click handler calling `canvas.drawImage(video)` triggered `CanvasPerformanceMonitor::WillProcessTask` because input dispatch occurred outside a Blink scheduler task. Both local and AP-renderer input now use the existing input task queue. Canvas task observers and microtask completion retain their task boundary.
2. Decoder readback succeeded while the displayed video rectangle stayed blank. Upstream `VideoPainter` normally expects a compositor video layer. The UEFI-specific patch records software video painting during ordinary document rendering.
3. Exceeding the decoded-frame quota before metadata publication triggered `HTMLMediaElement::MediaEngineError`'s ready-state assertion. Pre-metadata decode failures now use Blink's source-load failure state. The failed load releases decoded frames and decoder resources before a subsequent load.

Assertions remain enabled. The first two defects are retained in `media-first` and `media-second` evidence. The quota failure is retained in `media-final` evidence. Those runs are not passing release evidence.

## Verification

The tested EFI image SHA-256 is `ff22c9965c3dd72a2ef547a79f29d8d76299ef2a9750f7dd367cf5d4bcd9129b`. The build uses Chromium 158.0.8110.0 and musl 1.2.5. QEMU 11.1.2 uses KVM, OVMF, 4 GiB of guest RAM and either four vCPUs with an AP renderer or one vCPU with the local renderer. FFmpeg 9.0.2 on the host generates the test files; FFmpeg does not execute inside the firmware.

- The native Chromium adapter and EFI link complete. [Native build](media-release-native-build.log), [EFI link](media-release-link.log) and [binary validation](media-release-binary.log) retain the results. Binary validation reports no required unresolved symbols or host syscall instructions.
- `media` passes 20 assertions covering VP8 and VP9 decoded pixels, playback, pause, seek, rate, end, frame callbacks, loop, iframe decode/playback and reload. QMP injects a native mouse click. The host checks red, green and blue pixels at three positions in the actual QEMU framebuffer, separately from the page's canvas readback.
- `media-boundaries` passes 14 assertions covering capability reporting, HTTP failure, malformed input, unsupported audio, denied cross-origin loads, frame quota and successful decoding/readback after failures. Expected error events are required; timeouts and runtime aborts fail the test.
- Both media scenarios pass with four vCPUs and one vCPU. [Four-vCPU results](media-release-suite.json) and [one-vCPU results](media-release-single-suite.json) include the tested image hash.
- All seven four-vCPU scenarios pass: media playback, media failure/recovery, IndexedDB, Cache Storage, service-result collection, asynchronous input and tab/site interaction. The input fixture holds JavaScript for 12 seconds at a time and checks passive wheel input, cancelable event handling, root/nested keyboard scrolling, scrollbar dragging and editable/select controls. [Input results](media-release-scroll-input-scroll-input-results.json) retain the measurements. These are controlled-fixture results, not broad compatibility or latency guarantees.
- [Static checks](media-release-static-checks.json) record Python/JavaScript syntax, the applied upstream patch, source-mirror equality, diff whitespace and fixture hashes.

Actual framebuffer captures: [red](media-release-media-input-media-red.png), [green](media-release-media-input-media-green.png), [blue](media-release-media-input-media-blue.png).

The [service probe results](media-release-service-capabilities-service-capabilities-results.json) distinguish successful VP8 decoding and IndexedDB access from unavailable audio, dedicated workers, WebGL contexts and WebGPU adapters. Passing the probe runner means the results were collected; it does not mean every API works. The renderer still executes at CPL0. This change does not implement a security sandbox, worker execution or a GPU provider.

## Reproduction

Run from the project directory. Start the fixture server in a separate terminal and stop it after the tests:

```sh
python3 tests/make-media-fixtures.py
python3 tests/network-servers.py
```

```sh
python3 build-blink-libraries.py --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 tests/state-suite.py --https-ca build/network-https/ca.esl --prefix media-release media media-boundaries indexeddb cache service-capabilities scroll-input tabs-site
python3 tests/state-suite.py --workers 1 --cpus 1 --https-ca build/network-https/ca.esl --prefix media-release-single media media-boundaries
```

`tests/input-pages/media.html` contains the playback assertions. `media-boundaries.html` contains the failure/recovery assertions. `tests/input-scenario.py` controls QEMU input and validates framebuffer colors. `tests/state-suite.py` isolates firmware disks and preserves logs, screenshots and the image hash. Successful browser runs also require firmware teardown to release the payload, stack and TLS.
