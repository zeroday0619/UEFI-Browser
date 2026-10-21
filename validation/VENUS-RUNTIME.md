# Mesa Venus firmware runtime

## Implemented path

Mesa 26.2.5 runs in an isolated pure-UEFI candidate linked against firmware
musl and the existing Chromium libc++/libc++abi libraries. The upstream Venus
ICD creates a host Vulkan instance and device, submits GPU work, maps its
results and destroys its resources. The QEMU fixture uses the NVIDIA RTX 5080
through virglrenderer. No guest Linux kernel or hosted Vulkan loader is used.

`build-venus-runtime.py` extracts the hash-pinned Mesa archive into a separate
source directory, applies `patches/mesa-venus-uefi.patch` and copies the
first-party renderer backend into that tree. The static archive contains 187
members. Meson disables DRM, window-system platforms, external compression
libraries and XML parsing. Common headless WSI objects remain in the archive,
but the Venus WSI platform is disabled. The compiler uses an empty sysroot and
explicit musl and Chromium libc++ include paths. Meson dependency fallbacks
are disabled. Python generators reuse Chromium's Mako, MarkupSafe and PyYAML
sources as build-only tools.

Configure-time link probes resolve `firmware_syscall` to an absolute symbol
without executing the programs. This definition is restricted to configuration
probes; the EFI image links the real dispatcher. These probes establish symbol
availability, not firmware syscall behavior.

### Firmware fixes

The first runtime attempt exposed incompatible C11 mutex constants: Mesa's
pthread emulation uses `mtx_plain=1`, while musl uses `mtx_plain=0`. The firmware
build now selects native musl C11 threads. Mesa's monotonic condition-variable
adapter uses the same mutex-pointer conversion as musl's native wrappers,
with compile-time size and alignment checks.

The next attempt reached a working host ring but aborted in Mesa's watchdog.
Mesa waits through `clock_nanosleep(CLOCK_MONOTONIC, ...)`; the firmware syscall
dispatcher previously returned `ENOSYS`. The wait loop therefore advanced
without the intended sleep. The dispatcher now implements relative and
absolute waits, validates clock/flags/time values and uses bounded firmware
stalls with cooperative yields. QEMU tests check elapsed time, absolute
deadlines, past deadlines and invalid arguments. The upstream ring wait and
watchdog remain enabled and unmodified.

### Transport and lifetime

`src/gpu/venus-runtime/renderer.c` implements the Mesa renderer interface:

- Pending submissions retain synchronization objects until completion.
  Out-of-order completion cannot decrease counters, and reset generations
  prevent old submissions from signaling reset objects.
- Wait-any/all and timeout handling poll transport completion. A transport
  failure changes the renderer to device-lost state.
- Vulkan allocation exports wait for the host allocation command before
  creating their blob. Metadata, sizes, alignment and mappings are validated.
- Flush/invalidate operations require coherent backing. External DMA-BUF and
  external synchronization support are not advertised.

`src/gpu/venus-runtime/transport.c` implements the seven operations in
`runtime/venus_transport.h`. It supports up to 16 Virtio-GPU contexts sharing
one physical queue and host-visible aperture. It copies commands into retained
DMA storage, supports 32 in-flight descriptor chains across contexts and checks
completion fence, context, descriptor index and length. Resources use HOST3D
blobs and a bounded first-fit allocator for the host-visible aperture.
Successful completions retire their DMA storage; malformed or unfinished
requests remain quarantined until reset. Context teardown stops its host ring
threads before unmapping and releasing its resources. It preserves sibling
contexts and their pending jobs. The last close acknowledges device reset
before freeing any quarantined storage. A reset timeout disables bus mastering and
aborts without releasing quarantined storage.

The original standalone image used this trusted interface in process. The
current protected candidate routes it through a generation-checked CPL3
broker and disconnects/reconnects the firmware device driver around queue
ownership. See [protected browser integration](VENUS-BROKER.md) for the newer
implementation and evidence. A separate standard VGA GOP framebuffer provides
preview output.

## Reproduction

Run from the project directory after preparing the existing musl and Chromium
C++ runtime artifacts. The shader builder uses the existing `glslc` tool.
The QEMU runner requires KVM, Venus-capable QEMU/virglrenderer, a usable host
Vulkan ICD and `/dev/dri/renderD128`.

```sh
set -e
python3 build-venus-runtime.py --jobs 4
ENABLE_VENUS_RUNTIME=1 ENABLE_RENDERER_CPL3=1 BUILD_DIRECTORY="$PWD/build/venus-runtime-efi" JOBS=4 ./build-musl.sh
python3 tests/venus-runtime-host.py
python3 tests/venus-transport-host.py
python3 tests/venus-runtime-link.py
python3 tests/venus-runtime-qemu.py --cpl3 --prefix venus-runtime-local
```

Use an unused prefix for each QEMU run. `--icd` selects another installed host
ICD. `--image` selects an existing EFI candidate. The runner creates a temporary
ESP and variable store, stops only its own VM and retains logs, JSON results
and a screenshot. `ENABLE_VENUS_RUNTIME` rejects combination with Blink.
New Venus builds require the CPL3 broker, which supplies device cache
attributes. `ENABLE_RENDERER_CPL3=0` is rejected; pass `--cpl3` to the probe runner.
`ENABLE_VENUS_GPU=1` selects the separate CPL3 browser candidate described in
`VENUS-BROKER.md`. The table below records the earlier standalone image.

## Recorded validation

The recorded build used Debian Clang 23.1.3 and Meson 1.12.1. Execution used
QEMU 11.1.2, virglrenderer 1.3.0, NVIDIA 615.78.08 and an RTX 5080. Upstream
generated protocol code and musl headers emit compiler warnings; the builds
complete, but they are not warning-free.

| Check | Result | Boundary |
| --- | --- | --- |
| Static runtime and EFI build | Pass | Optional standalone candidate |
| ASan/UBSan backend checks | Pass | Injected completion, lifetime, allocation and failure scenarios |
| ASan/UBSan transport checks | Pass | Actual descriptor/aperture code with simulated DMA memory; no privileged PCI I/O |
| ICD link closure | Pass | Reachable ICD code resolves to the eight declared firmware service symbols |
| QEMU runtime | Pass, three lifecycles | Instance, device, queue, fence, timestamp, allocation, shader, image, mapping and destruction |
| Buffer readback | Pass, 65,536 bytes per lifecycle | GPU-written fill pattern and ordered nonzero timestamps |
| Shader image | Pass, 128 × 128 RGBA per lifecycle | 4,050 interior pixels, 11,182 background pixels and alpha checks |
| Displayed image | Pass, 1280 × 800 QMP screenshot | Independent pixel comparison of the scaled preview |
| Transport teardown | Pass, 51/51 completions per lifecycle | Device reset and DMA release after each lifecycle |
| Default browser EFI | Unchanged | SHA-256 `94d711c0fe3942329da0313acd8512a46444cc1b60d5373c79111c64f021d978` |

The transport host test covers copied command ownership, out-of-order
completion, 16-bit index wrap, incorrect fence/context/flags/length/index,
excess used entries, device reset requests, valid error replies, quarantine
and aperture overlap/exhaustion. It does not establish malicious-device
resistance or complete DMA isolation. The QEMU test exercises normal host
execution and acknowledged reset; it does not inject a hardware reset timeout.

`tests/venus-runtime-link.py` retains all three ICD entrypoints and expects
exactly eight unresolved strong symbols at the library boundary:
`firmware_syscall` and the seven `firmware_venus_*` functions. It rejects
additional imports, missing entrypoints, incorrect ELF targets and versioned
glibc symbols. The bootable candidate supplies these eight implementations.

Evidence: `venus-runtime-final.json`, `venus-runtime-final.log`,
`venus-runtime-final-pixels.json`, `venus-runtime-final.png`,
`venus-runtime-build.log`, `venus-runtime-efi-build.log`,
`venus-runtime-host.log`, `venus-transport-host.log`,
`venus-runtime-link.json` and `venus-runtime-sources.json`.
Earlier failed runs remain available, including `venus-runtime-ring-wait.log`
for the missing monotonic sleep. The earlier manual protocol probe is documented
separately in [VENUS-RENDERING.md](VENUS-RENDERING.md).

## Remaining integration

The newer CPL3 browser candidate connects ANGLE/Dawn and a protected transport
broker; see `VENUS-BROKER.md`. Skia GPU integration, asynchronous presentation,
comprehensive resource pressure handling, device-loss recovery and GPU
conformance remain unfinished.
The tested preview copies a GPU image to the independent GOP framebuffer on
the CPU; it does not implement direct GPU scanout. The normal browser still
uses its existing software GPU paths. No full Chromium or sandbox completion
claim follows from this isolated hardware runtime test.

The pinned Mesa source retains original copyright and license headers.
Bundled BLAKE3 and xxHash notices are listed in `THIRD_PARTY_NOTICES.md`.
The standalone probe's generated protocol headers remain unmodified.
