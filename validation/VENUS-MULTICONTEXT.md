# Concurrent Venus contexts and navigation cleanup

## Implementation

The optional Mesa Venus browser now separates physical device ownership from
Vulkan instance lifetime. Up to 16 contexts share one Virtio control queue,
32 in-flight descriptor chains and one host-visible aperture. Context IDs are
unique until the physical device resets. Resource IDs and fence tickets use
shared counters, while operations validate the creating context.

The CPL3 broker issues generation-checked session handles and tracks each
blob's owner. The 256-blob and 256-MiB budgets apply across all sessions.
Closing one session revokes only its user mappings. The transport retires its
pending command DMA, destroys its host context to stop ring threads, then
unmaps and unreferences its remaining resources. The final close resets the
physical device and returns it to the firmware driver. A lost transport retains
unacknowledged DMA until the last owner closes; sibling opens fail while lost.
The renderer-wide cleanup revokes all mappings before closing any session.

The navigation failure had two causes:

1. The old broker and transport admitted one context and always submitted
   commands with context ID 1. A live Dawn instance prevented ANGLE from
   initializing its Vulkan backend.
2. Destroying the Dawn wire server queued device-lost callbacks that retained
   native devices and adapters. Releasing the native instance without processing
   those callbacks left its Vulkan context alive after navigation.

`FirmwareGPUChannel::ReleaseNativeService` now processes the native event queue
between wire-server destruction and native-instance release. This cleanup does
not depend on DOM garbage collection. ANGLE may retain its display context
across pages; independent Dawn contexts can coexist with it and close normally.

## Reproduction

Run from the repository with the existing Chromium, Mesa and musl dependencies
prepared. These commands create a separate candidate image.

```sh
python3 build-blink-libraries.py --mozc --jobs 4
ENABLE_BLINK=1 ENABLE_VENUS_GPU=1 ENABLE_CPL3_PROBE=1 \
  ENABLE_BROKER_PROBE=1 BUILD_DIRECTORY="$PWD/build/venus-multicontext" \
  JOBS=4 ./build-musl.sh
python3 tests/network-servers.py --directory build/venus-multi-https
```

Keep the HTTPS fixture running. In another terminal:

```sh
QEMU_TEST_VENUS=1 VK_DRIVER_FILES=/etc/vulkan/icd.d/nvidia_icd.json \
  python3 tests/state-suite.py --workers 2 --cpus 4 \
  --https-ca build/venus-multi-https/ca.esl \
  --build-directory build/venus-multicontext --prefix venus-multi-local \
  --require-cpl3-probe --require-broker-probe --require-no-renderer-faults \
  gpu-lifecycle gpu-multicontext webgl webgl2 webgpu webgpu-canvas gpu-coherency
python3 tests/venus-transport-host.py
python3 tests/venus-broker-host.py
python3 tests/venus-runtime-host.py
```

Use an unused prefix to retain each run's logs and screenshots. The tests require
KVM, QEMU with Venus support, a working Vulkan ICD and `/dev/dri/renderD128`.

## Verification scope

The lifecycle fixture navigates between WebGPU and WebGL2 eight times in one
renderer. Each page checks actual GPU operations and displayed pixels. It also
requires at most two live transport contexts and a Dawn close on every departure.
The final WebGL page retains exactly one ANGLE context before browser shutdown.

The concurrent fixture runs two iframe WebGPU devices with WGSL compute
readback alongside three WebGL 1/2 canvases. It repeats operations eight times,
removes one frame, loses one WebGL context, creates another WebGPU frame and
checks surviving contexts after each operation. Three native transport contexts
are live concurrently: two Dawn instances and ANGLE. The suite checks visible
green and blue canvas pixels and balanced open/close counts after shutdown.

Host tests run the actual broker and transport source under ASan and UBSan.
They cover all 16 broker slots, stale handle reuse, the shared memory quota,
foreign resource and fence rejection, sibling survival, completion ordering,
malformed replies, context-before-resource destruction and DMA quarantine.
These tests simulate descriptors and grants; they do not execute privileged
PCI operations. The protected standalone QEMU probe checks the real mapping,
queue, readback, NX and alias boundaries separately.

## Results

The final browser candidate is `build/venus-multicontext/esp/EFI/BOOT/BOOTX64.EFI`,
SHA-256 `f4948d0a73f5f3e2400c7473b0af5cc4a0113eb83ee7d4db72cedf737d618093`.
The native Blink library and all three EFI candidates compiled successfully.
The environment used QEMU 11.1.2, Mesa Venus 26.2.5, virglrenderer 1.3.0,
NVIDIA RTX 5080 and Clang 23.1.3. Tests used KVM with guest PAT support.

| Evidence | Result |
| --- | --- |
| `venus-multi-final-suite.json`: lifecycle | Eight WebGPU/WebGL2 round trips; 9 contexts opened and closed, peak 2, zero remaining; displayed pixels checked on all 16 page loads |
| Same suite: concurrent contexts | 14 checks; two WebGPU iframe devices plus three WebGL canvases; 4 native contexts opened and closed, peak 3, zero remaining |
| Same suite: five GPU regressions | WebGL 1, WebGL 2, WebGPU, WebGPU canvas and 32 buffer-coherence cycles pass |
| `venus-multi-software-suite.json` | SwiftShader concurrent contexts and 32 buffer-coherence cycles pass |
| `venus-multi-protected.json` | Three real CPL3 lifecycles pass GPU fill, timestamp/readback, shader pixels, resource/session checks, NX/alias checks and firmware ownership restoration |
| `venus-multi-transport-host.log`, `venus-multi-broker-host.log`, `venus-multi-runtime-host.log` | ASan/UBSan tests pass |

Every hardware browser case verifies Mesa selection, valid context accounting,
zero remaining contexts, no transport loss, no unexpected renderer fault and
firmware controller restoration. Each browser case also passes the 14 CPU
boundary tests and all five 65-check broker preflights. The protected standalone
probe independently validates 4,050 triangle pixels and 11,182 background pixels.

`venus-multicontext-sources.json` records source and artifact hashes. Earlier
`venus-multi-first-*` results demonstrate that allowing multiple contexts alone
removed the immediate navigation failure but still accumulated Dawn contexts.
`venus-multi-events-*` verifies event cleanup before the final host-ring teardown
ordering change. The final suite covers that ordering change as well.

The default browser EFI remains unchanged. The verified candidate requires
`ENABLE_VENUS_GPU=1`; it has not replaced the default SwiftShader image.
Other host GPUs and long-duration stress tests were not exercised.

## Limits

The browser still reads GPU output back for CPU/Skia/GOP presentation. This
change does not add Skia GPU compositing, direct scanout, a separate GPU process,
physical-device loss recovery or complete WebGL/WebGPU conformance. The
16-session admission limit is covered by host tests; the browser scenario
executes three native contexts concurrently. Validation on other host GPUs
and long-duration resource exhaustion remains outstanding.
