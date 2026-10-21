# Skia Vulkan composition and QEMU presentation

## Implementation

This report records the earlier final-composition milestone. Canvas import changes and their separate A/B results are documented in [GPU canvas composition](GPU-CANVAS-COMPOSITION.md).

The opt-in `ENABLE_GPU_COMPOSITION=1` browser uses Skia Ganesh with the existing Mesa Venus runtime on the BSP. Validated CPL3 scroll tiles, clips, transforms, native browser controls and the pointer are drawn into Vulkan-backed Skia surfaces. The compositor submits a GPU image copy into one of two dedicated output allocations and polls its fence. Pending frames are coalesced before recording native UI drawing; completion triggers composition of the latest state. It does not call `readPixels`, map the tiled output allocation or copy the final frame into GOP memory.

The CPL3 renderer still rasterizes tiles and publishes bounded serialized pixels and scroll metadata. It does not supply Skia objects or Vulkan handles to this compositor. In the baseline validated in this report, WebGL/WebGPU canvas readback and the renderer's fallback pixel publication remain in place. The later [direct GPU canvas candidate](GPU-CANVAS-COMPOSITION.md) removes normal supported canvas presentation readback. This change removes CPU work from final composition and presentation; it does not establish a completely GPU-resident rendering pipeline.

The BSP has its own Venus context. Its mapped upload allocations use supervisor-only, non-executable aliases with PAT attributes selected from the host mapping. Renderer contexts retain the broker's ownership checks and quotas. Scanout validates context ownership, resource bounds, dimensions, stride and offset. Context teardown disables its active scanout before releasing the resource. The GPU path disables the firmware GOP pointer observer before taking display-device ownership.

## Host protocol

Stock QEMU 11.1.2 imported the tested NVIDIA DMA-BUF without its DRM format modifier. `glEGLImageTargetTexture2DOES` returned `GL_INVALID_OPERATION`; the SDL window remained blank even though the guest GPU image contained the interface. A diagnostic modifier injection confirmed the cause. Final validation uses the source patch, without injection or `LD_PRELOAD`.

[The QEMU patch](../patches/qemu-uefi-scanout-modifier.patch) adds `uefi-scanout-modifier=on`, disabled by default. It advertises private feature bit 15. Only when negotiated, `SET_SCANOUT_BLOB` carries an eight-byte little-endian DRM modifier after its standard 96-byte payload. The host passes the modifier to its DMA-BUF import. The negotiated path finishes the host GL update before acknowledging the flush so the guest can reuse the previous output buffer. This is a project-specific extension, not a standard Virtio feature. It must be reviewed for conflicts before rebasing QEMU.

The tested format is single-plane BGRA8 with NVIDIA modifier `0x0300000000606015`. Other GPUs, modifiers, multi-plane formats, HDR, multiple displays and direct physical hardware presentation are unverified. Host GL completion is synchronous; guest submission and fence polling do not make the entire host display pipeline asynchronous. GPU loss and hangs do not have a complete recovery path.

## Build and launch

The tested host uses QEMU 11.1.2, virglrenderer 1.3.0, Mesa Venus 26.2.5, NVIDIA driver 615.78.08 and an RTX 5080. Tests use KVM with four vCPUs, 4 GiB guest RAM, a 512 MiB host-visible GPU aperture and a 1280×800 SDL/X11 EGL window.

Unpack the QEMU 11.1.2 source matching `revisions.json`. Provide its normal host build dependencies, including Meson, Ninja, Python build packages, GLib, Pixman, SDL2, libepoxy, virglrenderer and libslirp development files. The build script patches that source tree and writes a separate executable; it does not install or replace system QEMU. `PKG_CONFIG_PATH` can select locally extracted development files. Debian's stripped source also needs the configure-time Python wheels; the recorded build placed `pycotap==1.3.1` in `python/wheels`.

```sh
QEMU_SOURCE=/path/to/qemu-11.1.2 JOBS=4 ./build-qemu-scanout.sh
python3 build-venus-runtime.py --jobs 4
python3 build-blink-libraries.py --mozc --jobs 4
ENABLE_BLINK=1 ENABLE_VENUS_GPU=1 ENABLE_GPU_COMPOSITION=1 \
  ENABLE_CPL3_PROBE=1 ENABLE_BROKER_PROBE=1 JOBS=4 \
  BUILD_DIRECTORY="$PWD/build/venus-compositor" ./build-musl.sh

BUILD_DIRECTORY="$PWD/build/venus-compositor" QEMU_GPU_COMPOSITION=1 \
  QEMU_ACCEL=kvm VK_DRIVER_FILES=/etc/vulkan/icd.d/nvidia_icd.json \
  ./run-qemu.sh
```

`QEMU_BINARY` overrides the patched executable location. The GPU launcher defaults to `build/qemu-scanout/qemu-system-x86_64`. It requires the Debian OVMF and SeaBIOS file paths used by the existing runner. The default `build/blink` EFI image remains unchanged.

## Validation

The QEMU harness captures the actual SDL window because QMP `screendump` has no software surface for DMA-BUF scanout in this QEMU version. Capturing a host window reads pixels for test evidence; that is outside the guest presentation implementation. A nonuniform-interface check rejects the black-window failure. Screenshots require visual inspection as well as log checks.

```sh
python3 tests/network-servers.py --directory build/venus-compositor-https
# Run the following in a second terminal while the fixture server is active.
QEMU_BINARY="$PWD/build/qemu-scanout/qemu-system-x86_64" \
  QEMU_TEST_VENUS=1 QEMU_TEST_GPU_COMPOSITION=1 \
  VK_DRIVER_FILES=/etc/vulkan/icd.d/nvidia_icd.json \
  python3 tests/state-suite.py --workers 1 --cpus 4 \
  --https-ca build/venus-compositor-https/ca.esl \
  --build-directory build/venus-compositor --prefix venus-compositor-verified \
  --require-cpl3-probe --require-broker-probe --require-no-renderer-faults \
  heavy-site scroll-input gpu-multicontext gpu-lifecycle

python3 tests/venus-transport-host.py
python3 tests/venus-runtime-host.py
python3 tests/venus-broker-host.py
```

The transport and runtime host tests use ASan/UBSan. They cover ownership and bounds failures, negotiated modifiers, exported device-local allocations, context cleanup and the existing broker boundary. See the retained `venus-compositor-*-host.log` files and [QEMU suite results](venus-compositor-verified-suite.json).

### QEMU results on 2026-10-21

| Scenario | Result | Evidence |
| --- | --- | --- |
| `heavy-site` | Passed | HTTPS `zeroday0619.dev`, visible theme click and Space response, wheel scroll and input exercised while loading |
| `scroll-input` | Passed | Input and composition during 12-second JavaScript tasks; checked cancellation and post-task reconciliation |
| `gpu-multicontext` | Passed | Peak four contexts including the BSP compositor; five opened and five closed; zero remaining |
| `gpu-lifecycle` | Passed | Eight WebGPU/WebGL2 round trips; ten contexts opened and closed; zero remaining |

Every case passed the 65-check broker probe and the 14 CPL3 boundary cases, recorded no unexpected renderer fault and returned to firmware. The candidate EFI SHA-256 is `14ead56435ead1357503b0d9f570a495679e57e433092674628212ecb431b4dc`. The [artifact manifest](venus-compositor-manifest.json) records source, host executable and baseline image hashes. The original default EFI hash was unchanged.

### Input and presentation measurements

A CPU comparison image was built from the same implementation with `ENABLE_GPU_COMPOSITION=0`. Both paths used the primary Virtio VGA device, SDL/X11 capture, KVM and four vCPUs. The runs were sequential. The CPU run passed the same real-site and long-JavaScript input scenarios. [Raw comparison data](venus-compositor-comparison.json) retains presentation counts and timing totals.

| Measurement | CPU output | GPU output |
| --- | ---: | ---: |
| First keyboard-scroll capture during a 12-second JavaScript task | 67.86 ms | 66.80 ms |
| Real-site theme click capture | 1354.51 ms | 1760.60 ms |
| Real-site Space-key theme capture | 2040.20 ms | 2018.42 ms |
| Mean native presentation call, real-site scenario | 1618.39 µs | 1096.37 µs |
| Mean native presentation call, scroll scenario | 1634.14 µs | 1008.47 µs |

These single trials do not establish an overall responsiveness improvement. The GPU path presents pointer updates through full GPU composition; the CPU path has a separate framebuffer cursor. Presentation-call counts therefore differ, and their means are not equivalent per-frame workloads. The CPU timings also exclude asynchronous GPU completion. Window-capture timing includes QMP delivery and capture overhead, not optical display latency. The implemented change is GPU final composition and direct presentation without a final-frame CPU readback or GOP copy. Renderer rasterization, page scripting and existing GPU-canvas readback remain.

The long-task scenario verifies root and nested keyboard scrolling, passive wheel scrolling and both scrollbar drags before JavaScript completes. Cancelable wheel/key listeners, editable fields and select controls preserve their semantics. Final GPU reconciliation had zero differing channels in the checked regions. All tested page handlers still execute on the renderer; event handlers cannot execute concurrently with its long JavaScript task.

### Changed components

| Path | Change |
| --- | --- |
| `src/blink/gpu_compositor.{h,cc}`, `config/blink-navigation.gn` | Skia Vulkan context, allocator, surfaces, output images and fence polling |
| `src/blink/browser_entry.cc`, `src/blink/remote_frame.cc` | GPU composition of the validated scene, browser UI and pointer; frame coalescing |
| `src/gpu/venus-runtime/{proxy,renderer,transport}.c`, `runtime/venus_{device,transport}.h` | BSP context, exported allocation lookup, checked scanout and context teardown |
| `src/virtual_memory.{h,c}` | Supervisor device aliases with host-compatible cache attributes |
| `build-musl.sh`, `build-qemu-scanout.sh`, `run-qemu.sh` | Explicit build and launch switches; separate patched host executable |
| `patches/qemu-uefi-scanout-modifier.patch` | Negotiated modifier transport and host display completion |
| `tests/browser-qemu.py`, `tests/input-scenario.py`, `tests/state-suite.py`, Venus host tests | Actual GPU-window capture, input checks and ownership/lifecycle coverage |

The QEMU build script was rerun against an already patched tree. Forward-only application prevents `patch` from automatically reversing an installed patch. The resulting device still exposes `uefi-scanout-modifier`, and final tests use that rebuilt executable. Separate smoke tests also passed with system QEMU and CPU/GOP output, and with patched QEMU and GPU output. Both completed EFI loader cleanup. Modified source checks pass. A repository-wide whitespace check also reports pre-existing trailing whitespace in `patches/blink-build.patch` and a generated `validation/http-driver-build.log`; those unrelated contents were retained.

![Actual GPU display showing the website](venus-compositor-verified-heavy-site-input-heavy-click.png)
