# Broker allocation lookup

## Evidence and scope

The prior live YouTube run reached 180 seconds but failed to show the Inspector
Console prompt. Its failure stack stopped the BSP in `renderer_runtime_buffer()`
while validating an eight-byte allocation output pointer. The implementation
searched every tracked allocation for ownership, then searched them again for
backing-extent overlap when the pointer was outside the allocations. A stack
output pointer therefore incurred two full traversals before the existing page
permission checks. A single stack sample does not attribute the whole Console
delay to this path; the workload comparison below measures this specific cost.

## Implementation and security boundary

`src/process/allocation_index.h` maintains allocation pointers in ascending
address order. Ownership and overlap checks use binary search. Insertions and
removals update the index while the BSP dispatches broker requests; successful
recovery teardown clears it after AP quiescence. Metadata remains BSP-owned.
The existing 16,384-allocation quota bounds the index. Index updates are linear,
and the separate linked-list search for explicit free requests is unchanged.

Ownership still requires containment within the originally requested byte size.
The overlap check includes page-rounded backing, so unused padding cannot become
an alternative route through a page grant. A range starting in a gap is checked
against the next extent as well as the previous one. Address overflow is rejected.
The supervisor callback and BSP page-table walk still enforce present/read/write
permissions after lookup. No positive permission result is cached.

## Host validation

`python3 tests/broker-allocation-index.py` passed with ASan and UBSan. The test
compares the production index with a separate linear reference across randomized
insertion/deletion, all 16,384 slots, byte ends, page padding, gaps, adjacent
allocations, spanning requests, overflow and reset. The host timing record in
`broker-index-host.log` is diagnostic and is not a browser benchmark.

## QEMU comparison

`broker-index-final-comparison-build.json` records a control link that substitutes
only the previous `renderer_runtime.o`; all other link inputs, including the
startup/memory probes and loader, are shared with the indexed probe. Both use
QEMU 11.1.2/KVM, four virtual CPUs, 4 GiB RAM and isolated disks. Boots ran
sequentially. Each boot produced seven probe samples including recovery; these
are not seven independent browser runs.

The timed workload holds 4,096 pool allocations and performs 2,048 zero-size
allocation requests with a valid stack output pointer. Each request must validate
the pointer, reject the zero size and preserve the output value. Allocation
creation/destruction and separate negative permission checks are outside timing.

| Image | Samples in microseconds | Median |
| --- | --- | --- |
| Linear control | 51581, 54784, 55400, 54895, 102102, 110624, 108313 | 55400 |
| Indexed | 772, 796, 791, 780, 1803, 796, 787 | 791 |

`broker-index-comparison.json` retains these measurements. Both final suites pass
65 broker checks on each invocation, all 14 CPL3 boundary cases, live fault
recovery, requested-size/padding denial, read-only and PROT_NONE denial, restored
write permission, freed-pointer rejection, dynamic reservations and sparse page
tables. These checks do not establish complete sandbox coverage.

The first combined probe failed in its HTTP response check because the startup
boundary loop ran before browser network initialization and did not advance
network jobs. `cpl3_probe.h` now initializes, processes and shuts down those jobs
when the broker probe is enabled. Both comparison images use this same harness
fix. `broker-index-control-suite.json` retains the initial failure and is not
counted as a passing result.

Source and image hashes are in `broker-index-sources.json`. Normal EFI SHA-256:
`bdbb439de390fc5a1d99ba5c70f81efbe04e4fc81ce0b0271f9bc3a13291232c`.
No Chromium native-library source changed. The default image is unchanged.

`broker-index-normal-suite.json` records six passing normal-image regressions:
DevTools (87.40 seconds), renderer scrolling (32.19), workers (21.16), WebGL
(21.06), WebGPU (21.34) and MSE audio (42.84). Every scenario ran with renderer
CPL3 and no unexpected renderer fault. Captured Opus audio passed the stereo,
continuity, mute and attenuation checks. `broker-index-binary.log` records static
EFI/ELF validation with no required unresolved symbols or hosted syscall/sysenter
instructions.

## Live-site validation and remaining latency

`broker-index-youtube-run.json` records a fresh raw-disk run of the normal image.
The observer reached 180 seconds, then opened DevTools through the native button,
selected Console and typed a player-state query with QEMU keyboard events.
The [Console screenshot](broker-index-youtube-inspected.png) and serial log show:

```text
YTSTATE 1 56.9335 3 null
```

The values are player state, video `currentTime`, `readyState` and `error`.
The player state uses the [YouTube playing value](https://developers.google.com/youtube/iframe_api_reference#Playback_status),
while ready state 3 means [HAVE_FUTURE_DATA](https://html.spec.whatwg.org/multipage/media.html#ready-states).
This is a point-in-time result, not evidence of uninterrupted or synchronized
playback. The [120-second](broker-index-youtube-120.png) and
[180-second](broker-index-youtube-180.png) captures also show video progression.

`broker-index-youtube-audio.json` records 222.175 seconds of 48 kHz stereo PCM;
178 of 222 complete one-second windows have RMS above 1. The raw WAV remains in
`build/broker-index-youtube/audio-output.wav`, with a separate finalized
`audio-playable.wav` containing identical PCM. Raw RIFF sizes remain zero with
this QEMU backend. These measurements establish device output, not speaker output,
audio quality or A/V synchronization.

The observer exited with status 0. No renderer stop preceded the Console result.
After the harness sent F12 to quit, shutdown used the existing NMI termination
fallback (vector 2); the log then confirms private-address-space reclamation and
EFI payload/stack/TLS release. This stop is retained in the run record rather than
describing the entire log as fault-free.

Input remains slower than required: the frame summaries report maximum queue
delay of 486,899 microseconds and maximum dispatch time of 486,773 microseconds.
The Console screenshot also retains failed HTTP requests and unsupported telemetry
requests. Successful Console execution does not complete YouTube compatibility,
low-latency input, shutdown latency or the full Chromium port. Sustained player
time progression, seeking and A/V timing remain to be measured.

## Reproduction

```sh
python3 tests/broker-allocation-index.py
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/broker-index" ./build-musl.sh
ENABLE_BLINK=1 ENABLE_MEMORY_PROBE=1 ENABLE_BROKER_PROBE=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/broker-index-probe-final" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/broker-index" python3 validate-binary.py
```

Start `python3 tests/network-servers.py --directory build/broker-index-https`
in another terminal, then choose unused output prefixes:

```sh
python3 tests/state-suite.py --workers 1 --cpus 4 \
  --https-ca build/broker-index-https/ca.esl \
  --build-directory build/broker-index-probe-final --prefix broker-index-probe-repeat \
  --require-broker-probe --require-cpl3-probe --require-memory-probe sandbox
python3 tests/state-suite.py --workers 1 --cpus 4 \
  --https-ca build/broker-index-https/ca.esl \
  --build-directory build/broker-index --prefix broker-index-normal-repeat \
  --require-cpl3 --require-no-renderer-faults \
  devtools renderer-scroll workers webgl webgpu mse-audio
```

The control link command is retained separately and requires the saved old runtime
object. Rebuilding current sources alone produces the indexed implementation.
