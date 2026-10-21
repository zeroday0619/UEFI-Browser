# Media Source Extensions integration

This change connects Blink's original `WebMediaSourceImpl` and `WebSourceBufferImpl` to Chromium's `ChunkDemuxer` in the firmware player. The original MSE DOM implementation manages source buffers, append parsing, timestamp offsets, append windows, removal, sequence mode and end-of-stream transitions. The initial validation below covers video-only VP8/VP9. The subsequent [MSE audio implementation](MSE-AUDIO.md) adds mono/stereo Opus and synchronized audio/video. Neither fixture suite establishes YouTube playback.

## Runtime behavior

`src/blink/media_source_player.inc` supplies the MSE members of `Player` in `media_player.cc`. The player opens a real MediaSource attachment and reads compressed buffers asynchronously. Each read decodes one packet with the existing libvpx adapter and yields before another read. Demuxer callbacks are posted to the owning task sequence because callbacks may otherwise run under a demuxer lock.

The player retains the current decoded frame and approximately half a second of upcoming frames, subject to the existing 64 MiB frame allocation limit. Presented frames are released. Seeking resets decoder state and reads the compressed stream again, so a decoded frame need not remain resident for backward seeking. Demuxer compressed-buffer quotas, decoder scratch memory and paint caches are additional allocations; this is not a global memory quota across players. URL-only playback retains its previous whole-clip buffering behavior.

The playback clock stops when decoded data runs out and resumes when new appended data is decoded. A seek generation discards callbacks from superseded reads and seeks. Teardown invalidates weak callbacks before stopping the demuxer. On a media error the player stops the demuxer but retains its object until teardown because existing SourceBuffers hold references to it.

## Recorded validation

The final `build/mse` EFI SHA-256 is `8dff87fa56ffb1b9ae889b9fc6ec0394648e0f0506b963826f0314702cd5337a`. The [native build](mse-seek-native.log), [EFI link](mse-seek-link.log) and [static binary validation](mse-seek-binary.log) passed. Static validation found no required unresolved symbols, hosted syscall/sysenter instructions or PE imports.

The initial image `a75454a950f82ec79b3c0e6fb4ff18a52cd5145acfd91155d06ce3695aa3e32e` [passed these QEMU scenarios](mse-first-suite.json) with four vCPUs, KVM and CPL3 renderer execution, without unexpected renderer faults:

- `media-source`: 22 checks covering 127-byte appends, source ranges and duration, playback before endOfStream, underflow and append recovery, seek cancellation, buffer removal and reappend, end-of-stream, replay, reopening, VP9 reattachment and decode statistics. The harness injects a native mouse click and checks red, green and blue pixels at three positions in the QEMU framebuffer. Canvas pixel assertions independently verify decoded frames.
- `media`: all 20 existing URL playback checks.
- `media-boundaries`: all 14 existing media failure/recovery checks.

Actual screenshots: [red](mse-first-media-source-input-mse-red.png), [green](mse-first-media-source-input-mse-green.png), [blue](mse-first-media-source-input-mse-blue.png).

## Boundary correction and final image

The initial boundary test waited for a frame at zero after an append window removed all data before one second. It was corrected to wait for metadata and seek into the retained range. The failed fixture result remains in `mse-bounds-suite.json`; it is not passing evidence. The DevTools scenario in that same suite passed native button entry, Console/CSS mutation, debugger pause/step/resume, reopening and target detachment.

A subsequent test reproduced an actual player defect: seeking to the exact duration returned only EOF and never completed the seek. `mse-bounds-window-suite.json` retains that failure. The player now requests coded data one microsecond before the finite duration while preserving the requested presentation time. This decodes and displays the final frame and completes `seeked`.

The final [five-scenario suite](mse-seek-suite.json) passed all 85 assertions on the final EFI image with CPL3 execution and no unexpected renderer faults. Its 14 new boundary assertions cover partial-parser abort, append windows, sequence mode, VP8-to-VP9 changeType, exact-end seeking, malformed data and recovery, a 70-frame 640×480 clip whose cumulative decoded pixels exceed 64 MiB, backward seeking after frame recycling, pending-seek detachment, SourceBuffer removal and recovery. The 22-check MSE streaming test and existing URL video, media-boundary and PCM audio tests also passed. The audio scenario includes captured PCM waveform analysis; it does not validate compressed MSE audio.

The verified EFI and matching payload were copied byte-for-byte to `build/blink` after checking that no QEMU process used that default directory. Previous binaries were retained under `build/release-backups`. [Promotion hashes](mse-default-promotion.json) and [source hashes](mse-release-sources.json) identify the artifacts. The default runner now includes the DevTools toolbar entry and MSE video path.

## Reproduction

Run from the project directory. Start the fixture server in a separate terminal:

```sh
python3 tests/network-servers.py --directory build/mse-https
```

```sh
python3 tests/state-suite.py --cpus 4 --workers 2 \
  --build-directory build/mse --https-ca build/mse-https/ca.esl \
  --prefix mse-verification --require-cpl3 --require-no-renderer-faults \
  media-source media-source-boundaries media media-boundaries
```

## Remaining work

The subsequent [MSE audio validation](MSE-AUDIO.md) covers Opus and synchronized audio/video. Encrypted media, codecs beyond VP8/VP9 and Opus, media-in-worker behavior, live seek ranges, long-duration/network-adaptive playback and broad MSE conformance remain incomplete. Decoding and painting still run on the renderer sequence. A long synchronous JavaScript task can delay presentation. This path uses software decoding and software rendering; it does not implement hardware video acceleration or a GPU driver. Live YouTube moving video, audio output, seeking and sustained playback have not been verified.
