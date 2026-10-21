# Dynamic virtual reservation registry

## Failure boundary

The ordered-index implementation still rejected the 4,097th active reservation,
regardless of free virtual address space or available physical memory. Its host
capacity test confirmed that `empty_region()` returned `ENOMEM` at 4,096 entries.

The separate `ordered-regions-youtube` run stopped on a V8 young-object promotion
OOM at approximately 156 seconds. The renderer reclaimed 1,674,207,232 tracked
physical bytes after stopping. That evidence does not identify reservation-slot
exhaustion as the cause. In the subsequent `ordered-regions-oom` run, a hardware
breakpoint on `partition_alloc::internal::OnNoMemoryInternal` never fired before
QEMU exited; the Console prompt check failed instead. No pre-OOM registry count
was captured. This change removes a confirmed capacity limit, without claiming
to resolve either live-site failure.

## Implementation

`src/virtual_regions.h` allocates an initial 4,096-slot registry on demand and
doubles capacity when all slots are occupied. Region records and their ordered
index share one allocation. Slot numbers remain stable across growth. The index
uses 32-bit entries, with capacity bounded by positive signed 32-bit indices,
allocation-size overflow checks and the existing allocator budget.

Growth allocates and copies a replacement before publishing it. Failure leaves
the old records, ordered index and capacity intact and returns `ENOMEM`. Ordinary
unmapping retains metadata capacity for reuse. `virtual_memory_release()` frees
the registry after removing all mappings. The metadata allocator uses the existing
firmware/broker C heap rather than recursively allocating virtual mappings.

The 512 GiB virtual arena, physical broker quota, heap budget, page permissions,
address hints and MAP_FIXED semantics remain unchanged. The registry resides in
renderer-owned memory and does not replace supervisor validation of page mappings.

## Evidence

`dynamic-regions-host.log` records ASan/UBSan success for the production registry
against an independent bitmap model. Coverage includes 30,000 randomized mapping
and trimming operations, 10,000 simultaneous reservations across two growth
boundaries, stable slot contents, initial/growth allocation-failure injection,
hole reuse, containment, gap selection, overflow rejection and metadata cleanup.
The adjacent-reservation timings are host diagnostics, not browser-performance
measurements.

Normal EFI SHA-256:
`9d838c18d822f80f9d772d0a7cfbdae3e2f2c3d3b5193a087915e5d90d0ca046`.

Probe EFI SHA-256:
`1de727acf5ed263ce18e5dd045544eb5957df415a48754fa87c258ced700a285`.

`dynamic-regions-sources.json` records source and image identities. Both EFI links
passed. No Chromium native-library source changed. The default `build/blink`
image and profile were not replaced. Static binary validation passed with no required unresolved symbols or hosted
syscall/sysenter instructions. `dynamic-regions-probe-suite.json` passed all 14
CPL3 boundary cases with live renderer recovery. Seven probe invocations each
created and released 5,000 reservations and reused a hinted hole. The suite also
passed split/replacement neighbor preservation, physical backing reclamation and
2,304 sparse page-table mappings. The 5,000-reservation samples took 17,637–19,544
microseconds; these are workload diagnostics, not a controlled comparison with
the earlier 1,024-reservation probe.

`dynamic-regions-normal-suite.json` records six passing QEMU 11.1.2/KVM
scenarios: DevTools (88.32 seconds), asynchronous renderer scrolling (32.15),
workers (21.18), WebGL (21.07), WebGPU (21.36) and MSE audio (42.83). Every case
ran with renderer CPL3 and no unexpected renderer fault. The audio check verified
captured Opus stereo tones, DMA continuity, mute and attenuation. These are
subsystem regressions; they do not establish sustained YouTube playback or
complete sandbox coverage.

## Live YouTube observation

The `dynamic-regions-youtube` run used the normal image in a fresh raw disk with
four virtual CPUs and 4 GiB RAM. The observer reached 180 seconds without a
renderer stop or OOM log. The 60-second capture (`dynamic-regions-youtube-60.png`; local artifact)
and 180-second capture (`dynamic-regions-youtube-180.png`; local artifact) show different video
frames; the later capture also shows the title, channel and related-video text.
This establishes visible progression, not uninterrupted playback or a causal fix
for the previous OOM.

`dynamic-regions-youtube-audio.json` records 178.843 seconds of 48 kHz stereo PCM.
Of 178 complete one-second windows, 137 have RMS above 1. The raw capture remains
at `build/dynamic-regions-youtube/audio-output.wav`; a separate
`audio-playable.wav` contains identical PCM with finalized RIFF sizes. QEMU again
left the raw size fields zero. Signal presence does not establish continuous A/V
synchronization, speaker output or audio quality.

The observer exited with status 1 after the later Console-prompt deadline. The
frontend loaded, and the harness adjusted its click after the language banner
moved the tab strip. The failure screenshot (`dynamic-regions-youtube-console-ready.png`; local artifact)
places the cursor on Console, but the panel has no prompt. No player-time
expression was executed successfully.

`dynamic-regions-youtube-failure-backtrace.log` captures the BSP in
`renderer_runtime_buffer()` while servicing an allocation request with an
eight-byte output pointer. This path linearly searches tracked allocations and
their extents before validating supervisor permissions. One sample does not
establish that it caused the entire delay. The next diagnostic must measure the
frequency and duration of these searches and distinguish them from frontend
task/layout delays. Any lookup optimization must preserve requested-byte bounds,
padding rejection and current page permissions.

## Reproduction

```sh
python3 tests/virtual-regions-host.py
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/dynamic-regions" ./build-musl.sh
ENABLE_BLINK=1 ENABLE_MEMORY_PROBE=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/dynamic-regions-probe" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/dynamic-regions" python3 validate-binary.py
```

Start `python3 tests/network-servers.py --directory build/dynamic-regions-https`
in another terminal, then run with unused output prefixes:

```sh
python3 tests/state-suite.py --workers 1 --cpus 4 \
  --https-ca build/dynamic-regions-https/ca.esl \
  --build-directory build/dynamic-regions-probe --prefix dynamic-regions-probe-repeat \
  --require-cpl3-probe --require-memory-probe sandbox
python3 tests/state-suite.py --workers 1 --cpus 4 \
  --https-ca build/dynamic-regions-https/ca.esl \
  --build-directory build/dynamic-regions --prefix dynamic-regions-normal-repeat \
  --require-cpl3 --require-no-renderer-faults \
  devtools renderer-scroll workers webgl webgpu mse-audio
```
