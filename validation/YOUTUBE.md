# Live YouTube playback investigation

The latest [queued Opus run](MEDIA-QUEUE.md) completes playback, pause, resume
and seek measurements without a media error or renderer fault. Initial and
resumed progression ratios are 0.95569 and 0.96011. The overall 737-second test
still fails because its independent visible-tab measurement does not return;
it is not a completed live playback validation. The following sections retain
the earlier investigations and their image-specific results.

Live tests have initialized a YouTube VP9/Opus MediaSource, displayed decoded video and produced AC’97 PCM output in QEMU. Earlier images subsequently displayed a player error. The response-policy image below displays the video and populated page at 180 seconds, but continuous full-video playback, seeking and broad compatibility remain unverified.

## Reproduction and correction

The public video `https://www.youtube.com/watch?v=aqz-KE-bpKQ` was opened through the native address bar in CPL3. The first run reached MSE initialization and audio output, then aborted at `ScriptPromiseResolverBase::Dispose`. Symbolizing the creation stack with the matching ELF identified `PublicKeyCredential::isConditionalMediationAvailable`. The browser dropped the unbound `blink.mojom.Authenticator` receiver, leaving the caller's Promise unresolved until collection. `youtube-first.log` retains the failure.

`src/blink/authenticator.h` now binds the interface per frame through `BrowserBroker`. With no authenticator backend, platform/conditional availability queries settle as false and client capabilities describe unavailable backend features. Credential operations fail without inventing credentials. This change implements service availability and callback lifetime handling; CTAP devices, platform credential storage, passkey creation and authentication remain unimplemented. Blink's assertion was not removed.

The HTTPS `authenticator.html` fixture verifies six conditions: conditional availability, platform availability, capability reporting, 32 concurrent queries, child-frame availability and parent-service survival after eight frames are removed with queries in flight. The QEMU suite (`youtube-auth-suite.json`; local artifact) passed those checks and the 17-check MSE audio/video scenario with captured AC’97 waveform validation. Both ran at CPL3 without renderer faults. Native compilation, EFI linking and binary validation (`youtube-auth-binary.log`; local artifact) passed.

## Live result

The corrected image has EFI SHA-256 `ce338bf0756e4287dfc533762eef3f7a1d484957160c4fda36921383e663157d`. The second live run completed the 90-second observation and shutdown without a renderer abort. At 60 seconds after navigation, the framebuffer (`youtube-second-input-youtube-60.png`; local artifact) showed a decoded scene from the video. At 90 seconds, the framebuffer (`youtube-second-input-youtube-90.png`; local artifact) showed the player's error message. The surrounding page retained loading placeholders.

The captured AC’97 file contains 41.2365 seconds of stereo PCM at 48 kHz. Of its complete one-second windows, 37 contain nonzero samples; see audio measurements (`youtube-second-audio.json`; local artifact). This proves emulator output rather than a browser play-state flag. It does not establish physical sound output or perceptual audio/video synchronization. The raw capture remains in `build/youtube-second/audio-output.wav`; a separate normalized WAV container preserves its PCM bytes.

The log shows repeated HTTP 200 `application/vnd.yt-ump` responses of 123 bytes before the player error. No firmware media decode error or renderer abort accompanied that transition. The content and cause of those responses remain unresolved. Do not infer a codec, network, account or server-policy cause from response length alone. Seeking, continuous full-video playback and broad video/account/codec coverage still need validation.

A third live run used the native DevTools Console after the error. The page reported `playabilityStatus.status` as `OK` and the video element's `error` as `null`; see `youtube-inspect.log`. This rules out claiming an observed HTMLMediaElement decode error, but it does not identify the application-level failure. The Console also exposed a failed main JavaScript bundle. A separate direct HTTP fetch measured the current English bundle at 10,877,807 bytes, exceeding the existing 8 MiB response limit; `youtube-kevlar-size.json` records its URL, headers, size and digest.

The tested EFI and matching payload were promoted byte-for-byte to `build/blink` after checking that no QEMU instance used that directory and acquiring the default profile lock. Previous binaries remain under `build/release-backups`. Promotion hashes (`mse-audio-default-promotion.json`; local artifact) identify both versions.

## Diagnostic command

Use an isolated copy of the verified ESP and the normal CA store. The `youtube-observe` scenario records screenshots at 10, 30, 60 and 90 seconds. Its successful exit indicates completion of observation and teardown, not successful playback. `YOUTUBE_INSPECT=1` additionally opens DevTools to inspect the player state after observation.

```sh
BUILD_DIRECTORY="$PWD/build/youtube-second" \
  QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 QEMU_TEST_REQUIRE_CPL3=1 \
  QEMU_TEST_AUDIO_CAPTURE="$PWD/build/youtube-second/audio-output.wav" \
  INPUT_SCENARIO=youtube-observe python3 tests/browser-qemu.py
```

## Large-resource follow-up

The [large-resource change](LARGE-RESOURCES.md) separates upload and response bounds, grows unknown-length buffers on demand and corrects EDK II parsing of partially delivered TLS fragments. Its QEMU fixture, network-security and MSE regression suites pass. This resolves the reproduced resource-size limitation; live YouTube results must still be assessed separately.

The subsequent `youtube-tls` observation used EFI `cc3da64ec265b4ee658dcbbf9a4ab5b1efa256c109f3cbb41417bcbc3c15463f`. The main JavaScript bundle was received successfully at 10,877,847 bytes. At 90 seconds the search field had appeared, but the player remained loading; no `MSE: source initialized` event occurred in this run. The screenshot (`youtube-tls-input-youtube-90.png`; local artifact) and `youtube-tls.log` preserve that result. Earlier video/audio evidence applies to the earlier image only. The latest image does not establish sustained playback or eliminate the remaining application/platform failure.

That EFI and payload were promoted to `build/blink`; promotion hashes (`large-resources-default-promotion.json`; local artifact) retain the previous versions and their backups. The response-policy build below subsequently replaced them.

## Response-policy and timeout follow-up

[ORB/CORP integration](RESPONSE-POLICY.md) removes the blanket rejection of cross-origin images, stylesheets and classic scripts. Document/worker COEP subresource policies are enforced. A local six-second HTTPS response reproduced a separate EDK II timeout defect: the driver used a five-second constant instead of the configured ten-second wait. The corrected driver passes delayed-header/body and expiration/recovery tests. These changes are included in the default EFI `94d711c0fe3942329da0313acd8512a46444cc1b60d5373c79111c64f021d978`.

The `youtube-orb-timeout` observation received the 10,877,854-byte main script without a recorded firmware transport failure. The renderer initialized VP9/Opus and the AC’97 device. The 90-second image (`youtube-orb-timeout-youtube-90.png`; local artifact), 120-second image (`youtube-orb-timeout-youtube-120.png`; local artifact) and 180-second image (`youtube-orb-timeout-youtube-180.png`; local artifact) show changing video frames. At 180 seconds the YouTube header, video title, channel and related-video text were also present. This demonstrates progression during observation, not uninterrupted playback for the full 180 seconds.

`youtube-orb-timeout-audio.json` records 175.445 seconds of 48 kHz stereo PCM, with nonzero signal in 74 complete one-second windows. The raw capture is `build/youtube-orb-timeout/audio-output.wav`. QEMU left zero RIFF size fields; `audio-playable.wav` wraps the identical PCM in a finalized WAV header. These measurements establish device output, not physical speaker output, audio quality or continuous A/V synchronization.

The observer's later inspector stage failed because its fixed Console-tab coordinates assumed the language banner was visible. The saved screenshot shows a tab strip forty pixels higher with no banner. The diagnostic now locates the rendered selected-tab underline before clicking Console. That harness failure does not erase the preceding 180-second video observation, but no player-time result was obtained from that run.

The follow-up `youtube-orb-inspect` run also reached 180 seconds and loaded the DevTools shell, but its Console prompt remained unavailable after selecting coordinates from the rendered tab strip. `youtube-orb-inspect-youtube-console-ready.png` still shows Elements selected and the native cursor displaced from the requested Console location. Banner geometry alone therefore does not explain the remaining failure. Pointer delivery under this workload and frontend readiness require further diagnosis. Both observer runs exited with a diagnostic failure; neither is recorded as a complete YouTube or heavy-page DevTools pass. The independent local DevTools regression passed on the same EFI.

The site also requested four small `audio/mpeg` resources. The URL media player rejected those with `UEFI_MEDIA_FORMAT`; ordinary URL playback still supports the existing WAV and video-only WebM paths. Repeated 123-byte UMP responses remain unexplained, and keepalive requests to `play.google.com` still return unsupported errors. Neither the UMP length nor the MP3 failures establish the cause of every earlier player error.

## Subsequent raw-disk memory investigation

The subsequent [zero-rate and live-control investigation](MSE-ZERO-RATE.md) fixes
a local MSE DMA clock discontinuity. The live control retry measures a 0.2653
media-time/wall-time ratio and then exits before pause/resume/seek validation.
These results do not establish real-time or uninterrupted playback.

The later [broker allocation index](BROKER-ALLOCATION-INDEX.md) run reached
180 seconds and completed actual Console keyboard input. The query returned
`YTSTATE 1 56.9335 3 null`, and the captured image contains the executed expression
and result. Captured PCM and changing video frames provide additional evidence,
but continuous playback, seeking and synchronization remain unverified. The
observer exited successfully; its F12 cleanup used the existing NMI termination
fallback before releasing the renderer and EFI allocations. Input summaries still
show approximately 487 ms maximum queue/dispatch delays. The default image remains
unchanged.

[DYNAMIC-REGIONS.md](DYNAMIC-REGIONS.md) records the later dynamically sized
reservation registry and its fresh-disk live test. That image reached the
180-second observation with changing video frames and captured stereo PCM,
without an OOM or renderer-stop log. The subsequent Console-prompt check still
failed. The run does not establish uninterrupted playback, synchronization or
working heavy-page Console input, and the default image was not replaced.

[PAGE-RECLAMATION.md](PAGE-RECLAMATION.md) records two allocation-path fixes and their independent regressions. The `page-tables` image shows the live video surface at 120 seconds but then reaches the 2 GiB renderer data quota and recovers the renderer. This run does not establish sustained playback or a working live-site Console. Its serial evidence is `page-tables-youtube.log`, and the default image was not replaced.

## Watchdog lifetime and audio dispatch

[WATCHDOG-LIFETIME.md](WATCHDOG-LIFETIME.md) records a later 492-second run after
disabling the firmware boot watchdog. Console-mediated pause, resume and seek
to 120 seconds completed, but measured playback rates of 0.196 and 0.323 failed
the real-time assertions. The older exit and Console failures above remain
historical results for their specific images.

[AUDIO-DISPATCH.md](AUDIO-DISPATCH.md) records the subsequent removal of redundant
MSE read and Opus callback queue transitions, a controlled rendering workload
and the candidate's regression results. Neither change establishes complete
YouTube support or hardware media acceleration.

[MEDIA-BATCHING.md](MEDIA-BATCHING.md) records bounded compressed-packet batching
and its controlled regression. A fresh 497.66-second live run completed pause,
resume and seek without a media error, but playback ratios of 0.752 and 0.715
still failed the real-time checks. The Console required 31.87 seconds to become
ready. Long-task source attribution is tracked in
[MEDIA-TASK-DIAGNOSTICS.md](MEDIA-TASK-DIAGNOSTICS.md).

The [JIT index validation](JIT-INDEX.md) retains a subsequent 498.46-second
full live run. Pause, resume and seek complete, but the real-time gates still
fail. Faster permission transitions do not establish sustained playback.

[Cooperative scroll encoding](SCROLL-ENCODING.md) subsequently improves the
median timer interval in a controlled continuous-paint fixture from 243.8 ms to
8.2 ms while retaining nested/sticky scrolling and gutter/query regressions.
Its 497.94-second live run completes pause, resume and seek without a media
error or renderer fault, but playback ratios of 0.577 and 0.618 still fail the
real-time gates. The live page rejects the scroll scene before the inspector
opens; the exact rejection cause remains uninstrumented. The fixture result
does not establish real-time YouTube playback or hardware acceleration.

[Shared painting](SHARED-PAINT.md) then uses one Blink paint artifact for the
viewport and scroll metadata. Effect/iframe screenshots match the former path
pixel-for-pixel, and scroll/query/MSE regressions pass. Its 487.07-second live
run completes pause/resume/seek without a media error or renderer fault but
still fails the real-time gates at 0.684 and 0.731. The first scene rejection is
now identified as a nontranslation transform. Captured stereo PCM is nonzero;
physical audibility and audio/video synchronization remain unverified.

[Affine scroll composition](AFFINE-SCROLL.md) supports 2D matrices and orders
renderer input with absolute scroll updates. Transformed scroll fixtures pass
while JavaScript runs for 20 seconds. The 501.79-second live run moves the first
capture rejection to `clip-filter`, completes pause/resume/seek without a media
error or unexpected renderer fault, and records stereo PCM. Real-time gates
still fail at 0.679 and 0.707. Filtered scene support and media scheduling remain
open; the run does not establish improved live playback speed or hardware GPU
acceleration.


## Filtered scroll candidate

The `scroll-filter-youtube` run on image
`b21f120b6410b134df6008c351d0c027430a39e4f1440eaaaadaf55f4654bf7b`
completed in 500.10 seconds with CPL3 and no unexpected renderer fault. CSS
filter capture removes the earlier `clip-filter` rejection. The live page
published validated scenes before Inspector startup, but its unlocalized
blocking-wheel listener still prevents unrestricted asynchronous wheel input.

The playback probe reports no media error and passing pause/resume/seek checks.
Initial progression is 0.772843 times the page clock and fails the unchanged
real-time threshold. Resume is 0.962232; progression after seeking to 120 seconds
is 0.999991. The suite correctly exits 1. Captured stereo PCM is nonzero, but
physical audibility and audiovisual synchronization remain unverified.
See `SCROLL-FILTERS.md` for commands, screenshots, source identity, timing limits
and the remaining capture/encoding bottleneck. The default image was not
replaced.


[Cooperative scroll tile encoding](SCROLL-WIRE-SLICES.md) passes seven CPL3
scroll/media/DevTools regressions and improves timer fairness in the controlled
encoding fixture. The corrected live harness completes pause/resume/seek in
494.02 seconds with video and nonzero stereo PCM, but initial and resumed
playback rates remain below the real-time gate at 0.811 and 0.862. Capture and
picture serialization still block the renderer. The default image is unchanged;
sustained playback and all broader acceptance gaps remain open.


[Immutable drawing record reuse](RECORD-REUSE.md) removes redundant wrapper
allocation for matching single-drawing groups. A 600-panel controlled fixture
reduces recorded median capture from 35.260 to 22.847 ms, while timer p95 and
frame count remain unchanged. Seven CPL3 scroll/media/DevTools regressions pass
and five filter frames match the earlier image pixel-for-pixel. The first full
live run stopped at a false-positive Console readiness check. The corrected
`scroll-record-reuse-youtube-retry` completed playback control measurements in
499.14 seconds at CPL3 without renderer faults. Initial playback advanced at
0.826 of wall time, resumed playback at 0.858 and post-seek playback at 0.832.
Pause held and seeking completed without a media error, but both real-time
acceptance gates failed. No completion of the nine objectives is claimed.


## Visible-page playback measurement

`tests/youtube-playback.py` now adds an independent 20-second interval whose
endpoints run inside the visible video document. It records any visibility
transition and rejects a hidden, paused, seeking or errored final state. The
existing Inspector-inclusive progression and control gates remain unchanged;
all old and new gates must pass. This avoids treating tab-switch overhead as
the only possible explanation for slow playback.

`youtube-visible-measurement-suite.json` completed in 688.41 seconds at CPL3
without an unexpected renderer fault on image
`2e30b60b7318f443c9a474d7a35312877d6eb9715d8fbdf36d72c915e51c130b`.
The visible interval lasted 20.238 seconds, advanced media by 17.151 seconds
and decoded 512 frames. Its ratio was 0.847, with no visibility transition,
pause, seek or media error. Both Inspector-inclusive real-time checks also
failed. The visible measurement screenshot shows the live video in the browser.
Actual audiovisual synchronization and uninterrupted real-time playback remain
unverified. The source manifest and result JSON retain exact values.

`youtube-retry-stall-attribution.json` brackets 79 media-report intervals from
the preceding run with scheduler records. It retains pause and seek intervals
and identifies posting locations rather than exclusive CPU profiles. Long
renderer tasks coincide with exhausted audio look-ahead; the data does not
establish that any single posted callback accounts for all starvation.

## BSP-owned audio reservoir

[The retained PCM implementation](AUDIO-RESERVOIR.md) and its host-timed signal
checks demonstrate device output during a 1.5-second blocked JavaScript task.
The live `audio-reservoir-youtube` run on image
`4abfc0a59e1de06627884c2a4feae798c960a095532e661e6c650ffb6ae7d7da`
completes in 671.35 seconds at CPL3 without renderer faults. Its initial rate
of 0.891799 still fails the unchanged threshold, so the suite exits 1. Pause,
resume and seek pass. The visible 20.0106-second interval advances media by
18.447062 seconds, giving 0.921865 with 556 decoded frames and no media error.
The final frame was visually inspected. The recording does not establish
audiovisual synchronization or sustained full-speed playback. Long renderer
callbacks and video underflow still require independent media execution.

## Queued configuration candidate: successful visible interval

The subsequent `build/media-config` candidate passes the live YouTube scenario
in `media-config-youtube-suite.json` (700.55 seconds). Its playback report passes
all twelve checks. During a continuously visible 20.0116-second interval the
media clock advances 20.01754 seconds and 612 frames are decoded. The renderer
runs at CPL3 without unexpected faults; 29,000 packets are decoded by the media
service. The visible screenshot was inspected. Raw and normalized AC97 captures
are retained with signal statistics. See [configuration-transition validation](MEDIA-CONFIG-TRANSITION.md)
for the image identity, fixed fixture, checks and limitations. Audiovisual
synchronization and physical output remain unverified.
