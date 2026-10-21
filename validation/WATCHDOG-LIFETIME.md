# Firmware watchdog and live media controls

## Startup defect

The loader retained UEFI boot services but never disabled the boot watchdog.
The [UEFI 2.11 watchdog contract](https://uefi.org/specs/UEFI/2.11/07_Services_Boot_Services.html#efi-boot-services-setwatchdogtimer)
requires the boot manager to arm a five-minute timer before launching a boot
option. Timeout zero disables it. The checked-out EDK II `BmBoot.c` sets
`5 * 60`, and `WatchdogTimerDxe` calls `ResetSystem(EfiResetCold, EFI_TIMEOUT, ...)`
on expiration. Returning from an image or exiting boot services would cancel it,
but neither occurs while this browser is running.

Earlier `youtube-controls-index`, `youtube-controls-fixed` and
`youtube-controls-preflight` runs ended around that lifetime with QMP closure or
QEMU status zero. With `-no-reboot`, a firmware reset can terminate QEMU instead
of producing another boot screen. Those runs did not retain a firmware reset
reason, so their individual exit causes are not proven solely by the exit code.
The missing watchdog cancellation is a confirmed startup defect and explains
the common five-minute boundary.

`src/loader-post.h` now calls `SetWatchdogTimer(0, 0, 0, NULL)` before CPU/memory
checks and payload initialization. POST reports successful cancellation, skips
firmware that returns `EFI_UNSUPPORTED`, and stops on programming errors or a
missing service. The renderer's fault handling and NMI termination path remain
separate from this boot timer.

## Validation

The tested image is `build/media-timing-watchdog/esp/EFI/BOOT/BOOTX64.EFI`, SHA-256
`505f39d90ebdd2984a01d6503cad69d4984a80c6ae9679db8cef993d39001fcd`.
Source and binary hashes are recorded in `watchdog-media-sources.json`. Native
media-player/frame-loop compilation, browser archive construction, EFI linkage
and static binary validation passed. The default `build/blink` image is unchanged.

`post-watchdog-host.py` runs the production POST code with ASan and UBSan. It
checks Microsoft x64 ABI arguments, cancellation before allocation, unsupported
firmware, missing service and error propagation without payload setup.
`watchdog-post-qemu.json` records both normal POST and corrupted-ELF rejection
with the new cancellation check. The [POST screenshot](watchdog-post-success.png)
was visually inspected.

The `watchdog-media-suite.json` regression run also passed `mse-audio`
(43.64 seconds), `mse-rate` (25.94 seconds) and `renderer-scroll`
(32.18 seconds). All three ran at CPL3 without unexpected renderer faults
and required the successful POST watchdog-cancellation marker. The audio
scenario also passed its captured AC'97 output checks.

```sh
python3 tests/post-watchdog-host.py
python3 tests/post-qemu.py --build-directory build/media-timing-watchdog \
  --prefix watchdog-post-rerun --require-watchdog-off
BUILD_DIRECTORY="$PWD/build/media-timing-watchdog" python3 validate-binary.py
```

## Live control results

The isolated QEMU 11.1.2/KVM run used four CPUs, 4 GiB RAM, a fresh raw FAT disk,
CPU-reset logging and captured AC’97 output. It ran for **492.38 seconds** and
completed every control sample beyond the former five-minute boundary. The
observer then deliberately failed its real-time progression assertions; the
test cleanup stopped QEMU. This is not a passing overall playback benchmark.

Commands were typed into the real DevTools Console through QEMU keyboard events.
The video tab was selected during wait intervals and the inspector during
commands. The [Console capture](watchdog-youtube-input-youtube-playback-final.png)
shows pause, resume and seek commands and their returned samples.

| Check | Observed result |
| --- | --- |
| Initial progression | 4.13 media seconds in 21.12 page-clock seconds; ratio 0.196. |
| Pause | Position remained 75.0735 seconds over 11.6449 seconds; `paused` stayed true. |
| Resume | Position advanced 6.80 seconds over 21.0369 seconds; ratio 0.323. |
| Seek to 120 seconds | `seeking` became true, then false at 121.6745 seconds. |
| Progress after seek | Position reached 126.1545 seconds; no media error was reported. |

`watchdog-youtube-youtube-playback-results.json` retains all samples and checks.
`watchdog-youtube-run.json` retains exit code 1 and elapsed time. No unexpected
renderer stop or fatal media error occurred during these samples. The result
establishes these Console-mediated media operations, not pointer hit testing of
YouTube's controls, real-time playback, uninterrupted playback or A/V sync.

The final live harness changes from the previous investigation have now been
exercised: preliminary query acknowledgement, installation screenshot, repeated
sample collection, pause, resume and seek all completed. The earlier timeout
and process-exit records are preserved as failures.

## Timing evidence and remaining work

`frame_loop.cc` separates paint recording from scroll-scene capture and scrollbar
reflow variants. Media counters separate VPx decode from pixel allocation/copy,
record the shared-clock lead of video/audio buffers, and sample timer gaps.
Play resets the gap baseline so an explicit paused interval is not reported as
a timer stall. These are elapsed-time diagnostics and can perturb timing.

The live stream was 854 by 480. Across the approximately 428-second media sampling
span, cumulative VPx decode time was 2.324 seconds and pixel work was 5.131
seconds. Of 74 media samples, 68 had at most 20 ms of submitted audio ahead of
the clock. The maximum observed media timer gap was 20.456 seconds during
inspector startup. These observations identify buffer supply and task dispatch
as priorities for investigation; they do not alone distinguish waiting for
demuxed data from queued audio callbacks or synchronous main-thread work.

For the 1,602 records already exceeding 50 ms, median record time was 59.386 ms,
including 24.399 ms paint recording and 28.460 ms scroll capture. Scroll reflow
variants had a median of zero in the separately logged slow-scroll samples.
These are conditional slow-record statistics, not overall frame-time medians.
Raw samples and scope notes are in `watchdog-youtube-timings.json`.

The measured image posted read, output and completion work through the renderer
main sequence. The subsequent [audio dispatch change](AUDIO-DISPATCH.md) removes
redundant queue transitions and verifies the effect under continuous animation.
Distinguishing remaining waits from stream starvation and moving media work off
long main-thread tasks remain necessary. A complete
Chromium port, audited sandbox, hardware GPU driver and the other acceptance
items in [IMPLEMENTATION-GOALS.md](IMPLEMENTATION-GOALS.md) remain outstanding.
