# Independent CPL3 Opus decoder process

An experimental media process executes the existing Chromium Opus library on
a separate application processor while Blink/V8 is occupied. It has private
writable image data, TLS, stack, page tables, interrupt gates and trap/escape
state. The BSP owns its request page and lifecycle. The decoder uses fixed
private storage and copies compressed bytes before decoding. No new production
library is added.

With `ENABLE_MEDIA_SERVICE=1`, Chromium's `OpusAudioDecoder` now sends real
mono/stereo MSE decode requests to this process through the renderer syscall
broker. Chromium retains channel-mapping parsing, timestamps, pre-skip and
discard-padding logic. The service runs Opus multistream initialization,
gain/phase controls, reset and packet decoding in private storage. The feature
remains disabled by default and the default EFI image has not been replaced.

The initial bridge waited synchronously for each bounded transaction. The
subsequent [queued audio path](MEDIA-QUEUE.md) retains compressed MSE packets
and applies Chromium's discard/timestamp helper on the BSP, allowing decode
and PCM output to continue through a six-second renderer JavaScript block.
Network/demux replenishment and video presentation remain renderer-dependent.
The results below describe the earlier synchronous bridge unless explicitly
identified as queue validation.

## Implementation

`src/loader-image.h` shares the ELF layout and payload entry ABI.
`src/loader-media.c` instantiates the existing loader-side sandbox in a
separate translation unit. `src/loader-media-symbols.h` gives its assembly
entry points and exception state distinct symbols. Static supervisor metadata
also remains separate because each instance is compiled independently.
The service selects the second enabled AP after the BSP; Blink uses the first.
The implementation currently requires at least three enabled logical CPUs.
Tests use four virtual CPUs.

`RendererBootstrap.media` is available only to the privileged browser. The
sanitized user bootstrap clears it. Payload role 2 dispatches directly to
`src/process/media_service.c`, without Blink initialization or access to the
firmware service proxy. Its supervisor accepts only ABI discovery, permission
queries and exit. TLS changes, memory mapping/protection changes, firmware
broker calls and JIT transitions are denied.

The request contract accepts clear mono/stereo Opus at 48 kHz, packets up to
65536 bytes and at most 5760 decoded frames. Each of eight production decoder slots has a
131072-byte private workspace. The diagnostic decoder has a separate workspace. Compressed input and PCM staging use private
arrays. The BSP publishes requests with release ordering and consumes results
after acquire ordering. The diagnostic consumer checks fixed frame counts and
finite bounded PCM energy before accepting results. The production BSP keeps
stream ownership, generation handles, transaction tickets and expected channel
counts in private memory. It bounds the returned frame count before copying
PCM. Renderer pointers are checked before reading commands/packets or writing
results. Invalid output buffers do not consume a ready result. Closed handles
and consumed tickets cannot address a replacement stream or transaction.

Only one transaction is in flight; a busy submission or pending completion
returns `EAGAIN` to the renderer wrapper. The wrapper keeps stack copies alive
until completion. Each transaction has a ten-second service deadline. Renderer
teardown joins the media AP before freeing shared pages and clears its stream
handles. A cached join result prevents cleanup from closing the AP event twice.
The eight-stream quota is explicit and does not allocate unbounded decoder
state. The feature does not establish per-origin service isolation.

`runtime/firmware_media.h` defines the typed command and client API.
`src/process/media_opus_broker.h` implements pointer validation, bounded
completion copies and generation handles; `media_opus_child.h` owns private
multistream decoder slots. `src/process/media_service.c` routes client calls
through the existing syscall boundary and manages the separate AP lifecycle.
`patches/blink-opus-service.patch`, applied by `prepare-blink.py`, connects
Chromium's original decoder while retaining its discard/timestamp helper.
The disabled-service build returns `ENOSYS` and uses the existing local decoder;
service-start or decode errors in an enabled build do not silently fall back.

The supervisor uses the existing Microsoft x64 to System V payload entry
transition. Each instance has its own `setjmp`/escape state; the existing
assembly preserves nonvolatile integer/XMM registers and floating-point
control state across escape. Media faults return through the media instance's
exception state. AP completion must be acknowledged by UEFI MP services before
its stacks, mappings or shared request storage are freed. Termination retries
while initial AP setup has not yet armed the protected exception path.

## QEMU validation

`tests/input-pages/media-process.html` starts a 12-second synchronous JavaScript
task. The host then sends F7 twice to the diagnostic image. Each native action
creates a media process, initializes the real Opus decoder and decodes 100
20 ms stereo packets. The fixture packet contains 440 Hz and 660 Hz signals.
Each result must contain 960 frames and nonzero bounded PCM energy.

The service rejects an oversized packet and all five prohibited capability
requests, then deliberately reads its supervisor canary. The test requires a
CPL3 protection page fault (vector 14, error 5), joins the stopped AP and frees
its resources. The second native action creates a fresh service and repeats
the checks. Both completions must precede the end of the blocked JavaScript
task. A subsequent page click verifies that Blink survived the media faults.
The final harness also checks immediate termination after service startup.

This is a native-input/QEMU integration test. It does not invoke page functions
through a debugger. The packet is repeated as a decoder/lifecycle stress input;
it is not a codec-conformance corpus or a sustained audiovisual stream.

```sh
ENABLE_MEDIA_SERVICE_PROBE=1 BLINK_SOURCE_POSITIONS=eager \
  BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/media-process-final" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/media-process-final" python3 validate-binary.py
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/media-process-final \
  --https-ca build/inspector-timing-https/ca.esl --prefix media-process-final \
  --require-cpl3 --require-no-renderer-faults media-process devtools
```

The fixture server uses
`python3 tests/network-servers.py --directory build/inspector-timing-https`.
Use a fresh test prefix for each run. `ENABLE_MEDIA_SERVICE_PROBE=1` enables
the media service and the destructive F7 diagnostic entry; it is a test image.
`ENABLE_MEDIA_SERVICE=1` includes the service without this diagnostic trigger.
The default build leaves the experiment disabled, and the default EFI image
has not been replaced.

The final image SHA-256 is
`eba87fb0bb2a74221a91402c2240fb6aa46e58a3b184e2457b234bb343567ec6`.
`media-process-final-suite.json` records passing independent media (35.91 s)
and DevTools (87.27 s) scenarios at CPL3 without unexpected renderer faults.
Immediate AP termination completed in 61 ms. Both sets of 100 Opus packets and
both deliberate media protection faults completed between the JavaScript
begin/end serial markers. Each fault had error code 5, and all five prohibited
capabilities were rejected. The surviving page and its post-recovery input
result were visually inspected in the captured frame.

The earlier `media-process-capabilities` image passed the independent process
case (35.90 s), retained-audio case (27.17 s), MSE audio (42.60 s) and MSE rate
case (25.94 s), all at CPL3 without unexpected renderer faults. Its SHA-256 is
`a786da3ede1573bd2fc325b44c26b947a460d863e9f639f71e201f33882169a0`.
These audio regressions still exercise the original MSE path.

The separate `media-process-boundaries` image includes
`ENABLE_CPL3_PROBE=1`. Its sandbox scenario passed all 14 startup boundary
cases and live supervisor-read/page-table-write denial with two interactive
renderer recoveries in 34.24 seconds. The intentional renderer faults belong
to this negative test; the normal media-service scenario prohibits renderer
faults. Reproduce it with:

```sh
ENABLE_MEDIA_SERVICE_PROBE=1 ENABLE_CPL3_PROBE=1 \
  BLINK_SOURCE_POSITIONS=eager BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/media-process-boundaries" ./build-musl.sh
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/media-process-boundaries \
  --https-ca build/inspector-timing-https/ca.esl \
  --prefix media-process-boundaries --require-cpl3-probe sandbox
```

## Production MSE integration validation

Build and run the candidate with:

```sh
ENABLE_MEDIA_SERVICE=1 BLINK_SOURCE_POSITIONS=eager \
  BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/media-mse" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/media-mse" python3 validate-binary.py
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/media-mse \
  --https-ca build/inspector-timing-https/ca.esl --prefix media-mse-first \
  --require-cpl3 --require-no-renderer-faults --require-media-service \
  mse-audio mse-rate mse-reservoir
```

The native Chromium archive was rebuilt before linking. Image SHA-256
`86b214361c9991695e205c6cc878a6a2ded59c8bdc47f8eb0f38e09eaab4804d`
passed MSE audio (43.77 s), zero-rate/resume (26.14 s) and the retained-audio
scenario (28.14 s). Service logs report at least 1000, 100 and 200 production
packets respectively. The new `--require-media-service` gate requires a real
service stream and at least 100 decoded packets, so local-decoder fallback or
the small broker fixture alone cannot satisfy the gate. All cases enter CPL3
and report no unexpected renderer faults.

The MSE audio scenario checks playback, pause, seeking, separate source buffers
and audio-only playback, plus captured stereo tones, mute and attenuation. In
the 1.5-second blocking-script scenario, the host PCM observer measures a
1.00067 signal/wall-clock ratio over a 1.29790-second interior interval and a
40.60 ms maximum signal gap. This is host-captured PCM evidence; physical
speaker output and audiovisual synchronization were not measured. It verifies
the existing reservoir with the new decoding path, not an independent feed of
compressed data throughout arbitrarily long scripts.

`src/process/media_opus_probe.h` adds 32 service-boundary checks: pointer and
size rejection, invalid channel controls/mapping, eight-stream exhaustion,
mono/stereo decode, reset, corrupt packets, stale handles/tickets, retry after
an invalid output destination, and slot reuse with a new handle. It runs only
in `ENABLE_BROKER_PROBE=1` builds. Its result is separate from the existing 65
broker checks. The first diagnostic harness attempt stopped after all 32
checks passed because the combined count no longer matched the existing
startup gate. That attempt is retained as a failure; the corrected build
preserves both independently asserted counts.

The corrected diagnostic image
`9afb12cabafb6df97e0c6fc4a80e645e6caee133f1975356194485e3e91effa7`
passes all 65 original broker checks, all 32 media checks and the MSE audio
scenario in 44.03 seconds, with 1000 production packets and no unexpected
renderer fault. `media-mse-boundaries-final-suite.json` records both counts.
The disabled-service image
`d4727cefe6fae8bb4b52053b763a1aff531478f438013f5487c77feee106511f`
passes the same MSE audio/PCM scenario through the original decoder in 42.65
seconds. These times are functional test durations, not a controlled
performance comparison. The page screenshot was inspected and shows all 17
MSE audio checks passing.

For the diagnostic build, add `ENABLE_BROKER_PROBE=1` to the build command and
`--require-broker-probe` to the suite command. `ENABLE_MEDIA_SERVICE=0` selects
the disabled-service build; omit `--require-media-service` for that test.
All tested candidates passed `validate-binary.py`. No source staging or commit
was performed, and the default image retains SHA-256
`94d711c0fe3942329da0313acd8512a46444cc1b60d5373c79111c64f021d978`.

The production protocol also passes the separate fault/restart fixture in a
`ENABLE_MEDIA_SERVICE_PROBE=1` image (36.11 s,
SHA-256 `e2ed602c80b4b8136f9c3cc61eb639b698e12841c262653b788389218005b3d6`). Both 100-packet
cycles finish during the 12-second JavaScript task, and both deliberate
supervisor reads raise protection faults while the renderer survives.
`media-mse-faults-suite.json` records this result. The media-source malformed
input scenario passes in 32.79 seconds with the production service enabled.

The recovery regression exposed a separate shared-mailbox bug. A recovered
background tab returned from `RemoteFrame::Poll` before acknowledging its
initial publication. Because the serialized-scroll mailbox is shared by all
tabs, that publication could prevent the active tab from publishing its final
URL and frame. A later Reload sent an empty URL; the diagnostic serial trace
records `reload recovered=1 url=` and `navigate url=`. The earlier attempted
fix added a redundant recovered-state check after the existing early return
and did not fix the failure.

`RemoteFrame::Poll` now acknowledges and discards a recovery publication before
returning, without applying its document state. The regression explicitly
waits for the second tab's recovery publication to be discarded before Reload.
The original failed run, redundant-guard attempt and tracing run are retained
in `media-mse-lifecycle-suite.json`, `media-mse-recovery-suite.json` and
`media-mse-recovery-debug-suite.json` respectively.

The final mailbox fix passes the full renderer-recovery scenario in
72.65 seconds. All six renderer starts enter CPL3 and pass the original 65
broker checks and 32 media checks. The scenario covers forced termination,
page fault, invalid instruction and breakpoint recovery, reload and page
clicks, tab retention and broker allocation reclamation. These intentional
renderer faults are expected negative-test results, not a fault-free run.
The final image SHA-256 is
`b88b952a57bb63ae12818f9dc80e7c7445d56cb98cbb05f6e146c03cf7aa994b`.
See `media-mse-recovery-final-suite.json` and its serial/runner logs. Source
hashes, image identities and retained successful/failed attempts are recorded
in `media-mse-sources.json`; the incremental implementation is captured in
`media-mse-implementation.diff`.

To run this diagnostic candidate from the project directory:

```sh
QEMU_MEMORY=4096 QEMU_CPUS=4 \
  BUILD_DIRECTORY="$PWD/build/media-mse-recovery-final" ./run-qemu.sh
```

The same final image subsequently passes the complete MSE audio/PCM scenario
in 44.24 seconds with at least 1000 production decode packets, 65 original
broker checks, 32 media checks and no unexpected renderer fault. This links
normal playback evidence to the exact image that passed recovery, rather than
relying only on an earlier candidate. See `media-mse-current-audio-suite.json`.

## Remaining integration and security work

Move compressed packet queues, cancellation and presentation beyond the
renderer task loop, then measure live YouTube playback under long JavaScript.
Video decoding and presentation still run in the renderer. Exercise unexpected
service termination during a pending production request, sustained multi-stream
playback, codec fuzz corpora and resource pressure. Add stream-aware scheduling
and per-origin isolation. This decoder boundary does not audit all privileged
services or establish the full sandbox objective. No hardware acceleration or
new live YouTube performance result is claimed by this change.
