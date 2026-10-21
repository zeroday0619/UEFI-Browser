# Renderer broker validation

This report records the pre-CPL3 images identified below. Current browser builds default to CPL3; see [the current implementation and evidence](RENDERER-CPL3.md). Reproducing this historical broker probe requires `ENABLE_RENDERER_CPL3=0`.

The BSP now validates renderer requests before using their addresses in firmware
calls. This is service-boundary hardening. Blink, V8, ANGLE, Dawn and SwiftShader
still execute at CPL0. These changes do not contain a renderer that executes
arbitrary native code or modifies page tables.

## Implemented policy

| Interface | Enforcement |
| --- | --- |
| Request dispatch | Copy the operation and five arguments once. Copy nested file and socket argument arrays before dispatch. |
| Memory buffers | Require a non-overflowing range in a tracked renderer allocation, the loader-owned renderer stack/TLS/private data, or a read-only shared ELF segment. Check BSP page presence and write permission before access. |
| Allocation | Limit live broker allocations to 16,384 entries and 2 GiB. Accept only allocation types used by the runtime. Store firmware allocation results locally before copying the address out. |
| Free | Require the exact base, allocation kind and page count. Reject interior pointers, foreign pointers, double frees and pool/page type confusion. Require page allocations to be writable before passing them to firmware for release. |
| Protection | Limit broker `mprotect` to page-aligned subranges of owned page allocations. Reject unknown flags and writable-executable permissions. Renderer-local page-table operations remain outside this protection. |
| Memory discovery | Return aggregate physical/available page counts. Deny the renderer's raw firmware memory-map request. |
| Firmware variables | Allow `TlsCaCertificate` reads and `UefiBrowserSettings` reads/writes with their exact GUIDs. Validate settings size, attributes, version and field values. Deny other variables and CA writes. |
| File/socket/DNS/audio | Use broker-owned copies for paths, argument arrays, polling records, socket addresses, transfer buffers, DNS names and audio uploads. Firmware never receives the renderer's original output pointer on these paths. |
| Network jobs | Return monotonically increasing opaque IDs. Look up live jobs before accessing them. Reject released IDs and repeated response retrieval. |
| Response release | Match against the broker's response registry. Free the trusted saved response, ignoring renderer-supplied counts and lengths. Reject foreign or already released responses. |
| Teardown | Reset renderer DNS queries as well as file/socket/network/audio state. Reclaim tracked allocations after AP quiescence. |
| Compatibility table | Omit real firmware vendor/configuration pointers and console/protocol handles from the renderer's proxy system table. |

Copies prevent later changes to a shared pointer or length from changing an
in-progress firmware operation. They do not make a malicious input semantically
trustworthy. Existing HTTP, file-path, descriptor-owner and audio-format checks
still apply after copying.

Files are limited to the existing `/profile/` namespace. This is not per-origin
storage isolation. Browser settings writes remain an explicit renderer
capability. Network response bytes and frame/compositor messages still use
shared storage outside this broker policy.

## Adversarial probe

`ENABLE_BROKER_PROBE=1` adds a renderer startup probe with 59 assertions. It sends
real requests through the AP-to-BSP queue, including invalid/unmapped/overflowing
addresses, read-protected buffers and frees, foreign and stale frees, allocation-type
confusion, forbidden firmware variables, malformed nested service arguments,
an audio operation with overflowing high bits, forged job IDs and corrupted
response counts. It also exercises a successful HTTP transfer, response
ownership, exactly-once retrieval, cancellation and subsequent job allocation.

The probe needs the local HTTP fixture server. It is disabled in normal builds.
Its success marker explicitly states that CPL0 isolation remains incomplete.
The test runner requires this marker when `--require-broker-probe` is set.

Reproduction:

```sh
python3 tests/network-servers.py
```

In another terminal:

```sh
python3 tests/websocket-server.py
```

Build and run from a third terminal:

```sh
python3 build-blink-libraries.py --jobs 20
ENABLE_BLINK=1 ENABLE_BROKER_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/broker-probe" ./build-musl.sh
python3 tests/state-suite.py --build-directory build/broker-probe \
  --workers 2 --require-broker-probe \
  --https-ca build/network-https/ca.esl --prefix broker-regression \
  network-secure websocket indexeddb cache workers audio webgl webgpu \
  settings-reset renderer-recovery
```

Keep both fixture servers running across the suite. The HTTP server generates
the CA used by the WebSocket TLS server, so start HTTP first. Normal images omit
the startup probe:

```sh
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

## Recorded QEMU results

The final probe image SHA-256 is
`2a9b1ab4bbe6fe5bdb7fa10d5d92eb0f73f447bfa73ec44f84e98ad6acce9906`.
Its five-case suite (`broker-final-probe-suite.json`; local artifact) passed HTTPS network
operations, WebGPU, dedicated workers, WebSocket/WSS and renderer recovery.
Every startup passed 59 assertions. The recovery case recorded six successful
probe executions. The matching EFI and symbols are in `build/broker-probe`.

The final normal image SHA-256 is
`a726df93d8b490e5e91bda72012363031389a48b5565abeddeb32fadce7e8750`.
It contains no fixture-dependent startup probe. Its
HTTPS and WebGPU suite (`broker-final-production-suite.json`; local artifact) and
binary validation (`broker-final-production-binary.log`; local artifact) passed.
This is the image in `build/blink/esp/EFI/BOOT/BOOTX64.EFI`.

The final addition rejects `FREE_PAGES` while the allocation is read-protected
or read-only. The probe verifies rejection of a `PROT_NONE` free and successful
release after restoring write permission.

Earlier runs cover additional features and are attributed to their own images:

The initial 58-assertion probe image SHA-256 was
`bc0c4f4c12c9eb930f1cd782ee1ab8a80526e914f9e2b60078c2398a4d16d2b6`.

The ten-case regression run (`broker-regression-suite.json`; local artifact) passed nine cases:
HTTPS network operations, IndexedDB, Cache Storage, dedicated workers, captured
AC'97 audio output, WebGL, WebGPU, settings reset and renderer recovery. Each
startup completed all 58 broker assertions. The recovery scenario recorded six
successful probe executions, including renderer restarts.

That run's WebSocket case failed because its dedicated fixture server was not
running. After starting the server, the WebSocket retry (`broker-websocket-suite.json`; local artifact)
passed on the same image, including trusted WSS and untrusted-certificate rejection.
The wire log (`broker-websocket-wire.log`; local artifact) records those WebSocket exchanges.
The binary check (`broker-probe-binary.log`; local artifact) also passed.

The corresponding normal image
`d200dad5a18dfd28868268d3ae4e652946e8872498eb86189634fb7ed18ec080`
passed a six-case AP suite (`broker-production-suite.json`; local artifact) and a
three-case single-CPU suite (`broker-single-suite.json`; local artifact). These results precede
the final writable-page requirement for `FREE_PAGES`.

During initial debugging, the probe's `printf` succeeded but its message was
discarded when Blink changed stdout buffering with `setvbuf`. The entry point
now flushes stdout before that change, and the probe flushes its completion
marker. Test success requires the marker rather than inferring success from a
subsequent page load.

## Remaining security boundary

The renderer retains privileged identity mappings, direct CR3/INVLPG/MSR access,
renderer-managed page tables and privileged cooperative thread switching.
It can bypass these broker checks. The browser/renderer frame channel and
network response storage still expose trusted addresses.

A security sandbox requires a user-mode renderer, supervisor-owned page tables,
a restricted syscall entry with validated copy-in/copy-out, protected service
state, safe shared-frame publication and contained exception/NMI handling.
The single-CPU renderer also needs that privilege boundary. Existing CPL3 copy
workers do not isolate Blink or V8. No native-code sandbox escape resistance,
physical-hardware containment or complete Chromium sandbox has been established.
