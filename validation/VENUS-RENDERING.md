# UEFI Venus hardware rendering

## Implemented path

The opt-in standalone UEFI application now creates a Vulkan graphics pipeline
through the official Mesa Venus protocol. It renders a color-interpolated
triangle into a 128 by 128 RGBA8 image, copies that image into a host-visible,
coherent Vulkan buffer and exports the allocation as a Virtio HOST3D blob.
The UEFI driver maps the exported allocation into the host-visible PCI aperture
and copies the GPU result into private guest memory.

The graphics command buffer contains a render pass, vertex and fragment shader
execution, image and buffer barriers, an image-to-buffer transfer and GPU
timestamps. A fence on the graphics queue's timeline completes before CPU
readback. The ordinary context CPU timeline only confirms command dispatch;
it cannot replace GPU completion. Readback uses the coherent memory type
reported by the device, bounds the allocation and aperture range, and uses a
dedicated allocation for each image and buffer. The host renderer supplies
its existing Vulkan memory export path.

`src/gpu/venus/render.inc` implements resource creation, drawing, readback and
destruction. `src/gpu/venus/shaders/triangle.vert` and `triangle.frag` are
original project shaders. `tools/build-venus-shaders.py` compiles them with the
installed glslc tool and emits bounded SPIR-V arrays for the EFI build. No
shader compiler is linked into the guest. The tested compiler reports shaderc
2026.4-1 and glslang 16.4.0-1.

`src/gpu/venus/present.inc` discovers the separate standard VGA GOP framebuffer
before taking ownership of the Virtio GPU. It displays a four-times enlarged
copy of the actual GPU pixels by writing that independent framebuffer. It
supports the GOP RGB and BGR byte layouts and checks dimensions, stride and
framebuffer size. The display routine does not call the firmware's Virtio
queues after ownership transfer. This preview uses CPU readback and GOP output;
Virtio scanout and zero-copy presentation remain unfinished.

## Validation

The EFI SHA-256 is
`013169f6cd4d6d93d0767ef4bec3e455f4ff8c522228fb74c1ea3224cc4912b7`.
`venus-render-first.json` and `venus-render-final.json` record successful rendering on NVIDIA GeForce RTX
5080 through QEMU/KVM, the NVIDIA ICD and virglrenderer 1.3.0. The guest remains
pure UEFI. QEMU is 11.1.2 with `honor-guest-pat=on`.

Three context/device lifecycles each create, render, export, map and destroy
the resources. Each readback verifies 4,050 interior pixels against barycentric
color interpolation, 11,182 background pixels and alpha across the full image.
All three RGBA images have FNV-1a checksum `0x6ab6a763`. The test also validates
65,766 fences, the full 16-bit queue-index wrap, context reuse, reset
acknowledgement and DMA backing release. QEMU exits successfully with no stderr.

The QMP screenshot is checked independently of the guest's serial success
markers. `tests/venus-render-output.py` samples the displayed pixels, checks
triangle interpolation and background colors, and saves a PNG. The 1280 by 800
capture in `venus-render-first.png` was visually inspected. The final rerun
repeats all rendering, screenshot and cleanup checks with the same image.
`venus-render-sources.json` records source, shader, image and result hashes. Negative checks
reject a black frame and a corrupted interior pixel; their results are retained
in `venus-render-negative-pixels.json`.

The render-disabled Vulkan build compiles without warnings. The render-enabled
image rejects an ordinary Virtio GPU without Venus before DMA allocation and
resets it successfully (`venus-render-unsupported.json`). The command-buffer
adapter also passes its 13 rejection cases under ASan and UBSan. The EFI has
no import table. The default browser image is unchanged.

## Reproduction

The build requires the existing LLVM tools and an installed `glslc` executable.
The Mesa release archive and generated protocol revision are pinned in
`revisions.json`; `prepare-venus.py` checks the archive hash. Each QEMU test
prefix must be unused.

```sh
ENABLE_VENUS_COMMANDS=1 ENABLE_VENUS_RENDER=1 \
  BUILD_DIRECTORY="$PWD/build/venus-render" \
  bash tools/build-virtio-gpu-probe.sh
python3 tests/virtio-gpu-qemu.py \
  --image build/venus-render/esp/EFI/BOOT/BOOTX64.EFI \
  --render --icd /etc/vulkan/icd.d/nvidia_icd.json \
  --prefix venus-render-new-run
python3 tests/virtio-gpu-qemu.py \
  --image build/venus-render/esp/EFI/BOOT/BOOTX64.EFI \
  --render --backend software --prefix venus-render-unsupported-new-run
python3 tests/venus-stream-host.py
```

## Remaining work

This is hardware shader execution, rasterization, image transfer and a tested
preview. It does not connect hardware acceleration to Blink, ANGLE, Dawn or
Skia, and it is not a complete Mesa ICD port. The Vulkan common runtime,
asynchronous ring submission, general resource lifetimes and image export,
protected GPU broker, quotas, cancellation, device-loss recovery and browser
presentation remain incomplete. The fixed shader and image fixture does not
establish Vulkan or WebGPU conformance. The browser's ordinary WebGL/WebGPU
backend still uses SwiftShader. The preview also requires QEMU's separate
standard VGA display and is not a native NVIDIA display driver.
