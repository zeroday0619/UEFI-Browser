# MSE zero-rate clock retention

The subsequent [watchdog investigation](WATCHDOG-LIFETIME.md) completed live
pause/resume/seek sampling beyond five minutes after disabling the firmware boot
watchdog. Real-time progression still failed. [AUDIO-DISPATCH.md](AUDIO-DISPATCH.md)
records the follow-up callback-path optimization and its separate evidence.

## Reproduction and correction

The live-player review found that `StartMseAudio()` returned immediately for
playback rate zero without stopping an active firmware DMA stream. The media
clock returned its cached position while the device continued consuming queued
PCM. Restoring rate one exposed the advanced device position as a time jump.

The isolated `mse-rate-control` QEMU/KVM run reproduced this with the broker-index
image `bdbb439de390fc5a1d99ba5c70f81efbe04e4fc81ce0b0271f9bc3a13291232c`.
After a 1.2-second zero-rate interval and 50 ms at rate one, the clock moved from
0.515521 to 1.073667 seconds. The new fixture rejected the 0.558146-second jump.
The failure occurred at CPL3 without an unexpected renderer fault.

`src/blink/media_source_audio.inc` now stops an active DMA stream at rate zero,
retains the returned sample position and clears the started flag. The existing
stream-pause implementation retains unconsumed PCM, so rate one restarts at the
remaining samples. A failed DMA pause becomes a media error. Repeated zero-rate
calls do not repeatedly stop an already stopped stream.

This preserves the [HTML playback-rate semantics](https://html.spec.whatwg.org/multipage/media.html#playing-the-media-resource):
rate zero freezes the playback position without making the element paused.
This change does not implement pitch preservation or extend the supported
hardware sample-rate range.

## Validation

The fixed EFI image is `build/mse-rate-fixed/esp/EFI/BOOT/BOOTX64.EFI`, SHA-256
`af76325ea5bd2517da02aa70ed9ea39a6b7f1c22dc8715d5f007c30f22ed1ff6`.
Its native build compiled the media-player object and rebuilt the browser archive.
EFI linkage and `validate-binary.py` passed. The default EFI image was not changed.

The fixed `mse-rate` case passed in 27.35 seconds. The corresponding clock values
were 0.515708 and 0.555896 seconds, a 0.040188-second increment after the rate-one
transition. The test also checks `play()` at zero rate, repeated zero-rate
assignment and a second resume without timeline discontinuity. These values
measure the media clock; they are not physical audio latency measurements.

The production DMA queue host test passed with ASan and UBSan, including partial
pause/resume, ring wrap, underrun refill and reset. Evidence is retained in
`mse-rate-dma-host.log`, `mse-rate-control-suite.json`,
`mse-rate-fixed-suite.json` and the corresponding serial/runner logs.

All five fixed-image QEMU cases passed at CPL3 without an unexpected renderer
fault: zero rate (27.35 s), MSE audio (42.63 s), MSE video (32.39 s), PCM audio
(31.97 s) and DevTools (87.36 s). The audio cases independently checked captured
AC’97 stereo output, continuity, mute and attenuation. Source and image hashes
are retained in `mse-rate-sources.json`.

Start the existing fixture server, then run the isolated suite:

```sh
python3 tests/network-servers.py --directory build/media-rate-https
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --build-directory build/mse-rate-fixed \
  --https-ca build/media-rate-https/ca.esl \
  --prefix mse-rate-rerun --require-cpl3 --require-no-renderer-faults \
  mse-rate mse-audio media-source audio devtools
```

## Live-site measurement boundary

`tests/youtube-playback.py` extends the live observer with typed DevTools commands
for repeated media-time samples, pause, resume and a seek to 120 seconds. Results
retain both the page clock and media clock, state, frame counters and visibility.
The video tab is selected during wait intervals; command entry uses the inspector.
The test therefore measures intervals containing both foreground and background
playback. It does not establish physical A/V synchronization or visible player
control hit testing.

The first `youtube-controls-index` run displayed video at 180 seconds and returned
`YTSTATE 1 64.8735 3 null`. It then failed with a QMP `BrokenPipeError` while typing
the additional probe. No control samples were obtained. The runner did not retain
the emulator exit status, so the cause remains unresolved. The runner now prints
that status on failure. This failed attempt is retained separately and is not a
passing playback-control result.

The `youtube-controls-fixed` retry returned two samples. The page clock advanced
20.8044 seconds while media time advanced 5.52 seconds, a ratio of 0.2653. Decoded
frames increased from 782 to 948. Both samples reported rate one, no media error
and hidden visibility at the instant of Console evaluation; the intervening wait
selected the video tab. This interval fails the real-time progression check.
During the subsequent pause command, QEMU exited with status zero without a guest
shutdown message. Pause, resume and seek therefore remain unverified on the live
site. The runner now supports `QEMU_TEST_CPU_RESET_LOG=1` and retains QMP events
to distinguish reset/shutdown paths in subsequent runs.

The `youtube-controls-reset` diagnostic attempt omitted the preliminary player
query to reduce typing time. Its first control sample timed out while QEMU was
still running. The reset log retained only the eight initial CPU reset entries
(224 lines); no additional reset was recorded. This attempt did not reproduce
the earlier process exit and does not explain it. It also supplied no new control
samples. The final harness restores the preliminary query, explicitly waits for
its serial result and captures probe-installation/timeout screens. Those final
harness changes passed Python syntax checks but have not yet been rerun in QEMU.
