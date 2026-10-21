# Protected Venus browser integration

## Execution boundary

The optional `ENABLE_VENUS_GPU=1` browser links Mesa Venus 26.2.5 into the
CPL3 renderer. ANGLE and Dawn resolve Vulkan entrypoints through a static
selector. Ordinary images continue to select SwiftShader. Mesa's three ICD
loader exports are renamed in a separate browser archive to avoid collisions
with SwiftShader; the original Mesa archive remains unchanged.

The renderer sends fixed operation messages through runtime request 133.
`src/gpu/venus-runtime/proxy.c` exposes the transport API in CPL3, while
`broker.c` owns the device context on the BSP. Session generations replace
supervisor pointers. The broker copies command bytes before transport
submission, owns resource metadata and rejects invalid ranges, stale sessions,
foreign resource identifiers and premature fence release. The resource budget
is 256 blobs and 256 MiB shared across at most 16 active transport sessions.
Each blob and completion ticket belongs to its creating session. Closing one
session preserves the queues and mappings owned by its siblings.

Mapped HOST3D blobs receive page-aligned read/write user mappings with NX set.
These grants carry a device-memory flag. The page-table service rejects aliases
of device grants and rejects attempts to add execution permission. Teardown
revokes user mappings before destroying resources. After renderer quiescence,
the BSP also resets any remaining GPU context before releasing renderer memory.
The fixed preview operation exists only in the standalone probe build.

This boundary restricts firmware memory and device access; it does not validate
every Vulkan command. Venus protocol decoding and Vulkan execution still trust
QEMU, virglrenderer and the host driver. It is not a separate GPU process or
proof of complete GPU sandboxing. The transport now supports concurrent ANGLE
and Dawn instances. Hostile-host DMA, GPU reset failure and full conformance
remain outside the demonstrated boundary. See [context lifecycle validation](VENUS-MULTICONTEXT.md).

## Integration fixes

ANGLE previously selected its headless-window display, which requires
`VK_KHR_surface` and a window-system extension. The firmware runtime has no
Vulkan window system. UEFI now selects ANGLE's upstream surfaceless offscreen
display; no unsupported surface extensions are advertised. Browser presentation
still copies GPU results into the CPU/Skia/GOP path.

Mesa also exposed an uninitialized musl program-name pointer when executing at
CPL3. Firmware startup now initializes both program-name variables before C
runtime use. Runtime checks verify both values.

The transport disconnects the firmware driver for the selected PCI controller
before taking ownership of its Virtio queues. When the last context closes it acknowledges reset,
releases DMA storage and reconnects the firmware driver. This fixes the hang
seen when returning from a successful browser GPU test to the firmware console.
The independent standard VGA framebuffer remains available for browser output.

### Device memory cache attributes

A subsequent WebGPU run read zeros from one 64-byte input range while adjacent
ranges contained the expected values. The transport had logged the host's WC
cache type but discarded it; user mappings were always created as write-back.
The transport now preserves cache types, and the protected mapping service
selects the corresponding existing PAT entry. If firmware has no WC entry,
it selects UC rather than creating a WB alias. Unsupported host cache types
are rejected. Permission changes and revocation preserve leaf cache bits.
The helper's host tests cover all eight PAT indexes and missing/invalid types.

`venus-browser-boundary-trace-webgpu.log` retains the mismatched GPU values.
The same WebGPU checks pass after the cache change. A separate 32-cycle fixture
repeatedly overwrites, dispatches and maps the buffer, checking all 64 integers
on every cycle. New Venus builds require the CPL3 mapping service and reject an explicit
`ENABLE_RENDERER_CPL3=0`. The historical non-CPL3 image does not exercise this
protected mapping path; its previous results must not be treated as general
CPU/GPU coherence validation.

## Reproduction

Run from the project directory with the pinned Mesa runtime, musl and native
Chromium libraries prepared. The tests require KVM, a Venus-capable QEMU and
virglrenderer, a working host Vulkan ICD and `/dev/dri/renderD128`.
Use unique output prefixes and keep the HTTPS fixture running for the suite.

```sh
python3 build-blink-libraries.py --mozc --jobs 4
ENABLE_BLINK=1 ENABLE_VENUS_GPU=1 ENABLE_CPL3_PROBE=1 \
  ENABLE_BROKER_PROBE=1 BUILD_DIRECTORY="$PWD/build/venus-browser" \
  JOBS=4 ./build-musl.sh
python3 tests/network-servers.py --directory build/venus-local-https
```

In another terminal:

```sh
QEMU_TEST_VENUS=1 VK_DRIVER_FILES=/etc/vulkan/icd.d/nvidia_icd.json \
  python3 tests/state-suite.py --workers 1 --cpus 4 \
  --https-ca build/venus-local-https/ca.esl \
  --build-directory build/venus-browser --prefix venus-browser-local \
  --require-cpl3-probe --require-broker-probe --require-no-renderer-faults \
  webgl webgl2 webgpu webgpu-canvas gpu-coherency
```

The isolated protected runtime probe does not need an HTTP server:

```sh
ENABLE_VENUS_RUNTIME=1 ENABLE_RENDERER_CPL3=1 \
  BUILD_DIRECTORY="$PWD/build/venus-protected-efi" JOBS=4 ./build-musl.sh
python3 tests/venus-runtime-qemu.py --cpl3 \
  --image build/venus-protected-efi/esp/EFI/BOOT/BOOTX64.EFI \
  --prefix venus-protected-local
python3 tests/venus-broker-host.py
python3 tests/venus-transport-host.py
```

## Protected runtime evidence

`venus-protected-cache.json` records three CPL3 lifecycles on
`Virtio-GPU Venus (NVIDIA GeForce RTX 5080)`. Each lifecycle validates a
65,536-byte GPU fill, ordered timestamps, shader pixels, session/resource/range
and quota checks, invalid fence rejection, NX protection and alias rejection.
The transport completes all submitted fences and reconnects the firmware
driver after each lifecycle. The independent screenshot validates 4,050
triangle pixels and 11,182 background pixels. The tested EFI SHA-256 is
`ed8481da2fcdb3e26df6ed2c9644f4d5fd3980319254b082e528177f441600ed`.

The host broker and transport tests pass ASan/UBSan. They inject invalid
messages, failed grants, stale generations, queue wrap, malformed completions
and aperture exhaustion. They do not execute privileged PCI operations or
establish malicious-device resistance.

Earlier failed runs are retained. `venus-browser-ownership-suite.json` records
the original missing-surface WebGL failure. `venus-browser-offscreen-suite.json`
records a later fixture outage: the local HTTP server was absent during the
broker preflight. These failures are not successful GPU validation evidence.


## Earlier browser results and lifecycle failure

The hardware browser EFI is `build/venus-browser/esp/EFI/BOOT/BOOTX64.EFI`,
SHA-256 `7b3ced726757bd7e7427ac42b21737db2c2aef8a6fce245a11166732402b26d4`.
`venus-browser-verified-suite.json` records five successful cases: WebGL 1,
WebGL 2, WebGPU, canvas and 32 buffer-coherence cycles. Every case requires
explicit Mesa Venus selection, transport initialization and firmware controller
restoration. Earlier individual results remain in `venus-browser-cache-suite.json`
and `venus-browser-coherency-suite.json`. Each browser case also checks CPL3 execution, all 14 intentional CPU
boundary tests and the 65-check broker probe. The screenshot checks inspect
actual displayed pixels, and successful sessions return to the firmware menu.

| Case | Result | Checked behavior |
| --- | --- | --- |
| WebGL 1 | Pass, 6 checks | Context, clear/readback, GLSL, triangle and invalid-command handling |
| WebGL 2 | Pass, 8 checks | WebGL checks plus buffer copy and readback |
| WebGPU | Pass, 13 checks | WGSL compute/rendering, mapping, binding bounds, validation and device recreation |
| WebGPU canvas | Pass, 11 checks | Formats, alpha modes, resize, reconfiguration and displayed pixels |
| Buffer coherence | Pass, 32 cycles | All 64 integers checked after each overwrite, dispatch and map |
| SwiftShader regression | Pass | WebGL 1 and the same 32-cycle WebGPU test with the software backend |
| Earlier WebGPU-to-WebGL navigation | Failed in the earlier image | ANGLE could not create another Vulkan instance while the transport context remained occupied |

`venus-browser-lifecycle-suite.json` and its GPU lifecycle log retain the
navigation failure after correcting an earlier harness error. The harness
previously treated intentional preflight exceptions as live renderer failures;
it now examines faults after the completed boundary-test marker. It continues
to reject actual scenario exceptions and checks all required preflight cases.
The failure identified two defects: the transport admitted only one context,
and pending Dawn device-lost callbacks retained native devices after wire
server destruction. The newer [multi-context implementation](VENUS-MULTICONTEXT.md)
addresses both. It processes the native callbacks before releasing the Dawn
instance and gives each instance its own Virtio context. No forced context
reset disguises a retained native instance. The earlier failure logs remain
historical evidence for the previous image.

`venus-software-cache-suite.json` records the software regression with the
updated page-table code. The default browser EFI remains unchanged, with
SHA-256 `94d711c0fe3942329da0313acd8512a46444cc1b60d5373c79111c64f021d978`.
No full Chromium, GPU conformance or complete sandbox claim follows from these
local fixtures.

Source and artifact hashes are recorded in `venus-browser-sources.json`.
