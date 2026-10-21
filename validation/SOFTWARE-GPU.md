# Software GPU execution in UEFI

This implementation connects native Blink graphics APIs to ANGLE, Dawn and SwiftShader in Chromium 158.0.8110.0. GLSL and WGSL execute through the statically linked Vulkan driver and its Subzero JIT. QEMU supplies a GOP framebuffer; it does not provide a host Vulkan or OpenGL context to this path.

## Implementation

`src/blink/webgl_context.cc` connects WebGL 1/2 to Chromium's in-process GLES command buffer and ANGLE's Vulkan backend. The context type is preserved during initialization. Shared-image pool creation and deletion run on the GPU sequence. WebGL canvas painting reads pixels into an unaccelerated image because this embedder does not composite foreign texture layers.

`src/blink/gpu_context.cc` connects Blink's WebGPU interface to a Dawn wire client/server and native Vulkan device. Owned command storage survives reentrant replies. Individual wire allocations are limited to 1 MiB and pending command storage to 32 MiB. The service polls native completion and client callbacks through the existing event loop. Device creation enables Dawn's internal texture usage for browser-managed canvas copies without adding copy usage to the web-visible texture descriptor.

The UEFI `GPUCanvasContext` path allocates regular Dawn textures and copies submitted pixels to a map-readable buffer. Presentation retains a CPU image after the frame texture expires. Synchronous snapshots wait only for their own map future, avoiding unrelated JavaScript callback dispatch from painting. Readback dimensions are capped at 8192 per axis and 32 MiB per transfer, with a five-second wait deadline. This deadline cannot interrupt a native driver call or shader that does not return. The backend remains cooperative.

SwiftShader entry points are namespaced before static linking to prevent collisions with ANGLE's Volk dispatch variables. Generated shader memory uses private anonymous pages and the existing protection API; executable shared-memory mappings remain prohibited. The single-worker configuration uses two raster batches instead of sixteen to fit multiple devices within the runtime's existing 128 MiB tracked heap limit.

The runtime now implements `uname` for Chromium GPU initialization and emits bounded abort backtraces. Patches are retained for Chromium, ANGLE, Dawn and SwiftShader. Added license texts are listed in [the third-party notice inventory](../THIRD_PARTY_NOTICES.md); they do not complete the binary distribution license audit.

## Security boundary

Shader validation and bounded graphics buffers do not create a renderer security sandbox. `src/loader-renderer.h` copies the firmware page-table root and executes the renderer at CPL0. `runtime/renderer_runtime.h` passes trusted service entry points, and `src/virtual_memory.c` directly changes page tables from that runtime. The GPU services and JIT code execute within the same privileged renderer environment. The current configuration also disables V8's sandbox.

The existing CPL3 presentation workers execute fixed transfer routines. They do not run Blink, V8, ANGLE, Dawn or arbitrary shader JIT code. This change does not implement hostile-renderer containment, a separate isolated GPU process or site isolation. A complete boundary still requires unprivileged renderer execution, supervisor-only broker state and page tables, validated copy-based service calls, and fault/timeout containment across that boundary. Arbitrary renderer memory corruption remains outside the protection provided by the current implementation.

## Compatibility limits

This is a software execution path, not hardware acceleration or a complete Chromium GPU service. Native shared-image/mailbox imports, external/video texture interoperability, WebGPU worker/offscreen frame transfer, broad WebGL extensions, full color management and conformance suites have not been established. Shader compilation and execution can delay renderer tasks. The tested local fixtures do not establish compatibility with every graphics-heavy website. Physical hardware has not been tested.

## Source revisions

| Input | Pinned revision |
| --- | --- |
| Chromium | `f12c350cb1e1e7ea05c11d6045b0a04158ec9e28` |
| ANGLE | `7865acacbf6380ce2acc54f5afd86a242cd239eb` |
| Dawn | `4a4e5d06e90992e50100df54d5d4475a6bf4a7ef` |
| SwiftShader | `cddb5c91b23ff70a1f607ea31610aa0d7f4b5ad5` |

The graphics dependency revisions come from the pinned Chromium gitlinks. `prepare-blink.py --resolve-inputs` fetches these sources and applies the retained patches. Host generators remain host programs; the graphics libraries are built with the firmware toolchain.

## Reproduced failures and fixes

- A static `vkGetInstanceProcAddr` export collided with ANGLE's dispatch variable. Namespaced driver exports remove the collision.
- Subzero tried to allocate executable private mappings through a shared-memory descriptor. Private anonymous executable pages use the runtime's supported JIT path.
- WebGL initialization reached an unsupported `uname` call and unimplemented shared-image pool operations. The runtime and in-process pool implementation now supply them.
- GLES context initialization used a generic OpenGL ES context for Blink WebGL. Explicit WebGL 1/2 context types preserve command validation and framebuffer behavior.
- Pixel readback passed while the GOP canvas remained blank because foreign GPU layers were not painted. CPU canvas snapshots supply the actual displayed pixels.
- ANGLE context disposal asserted when Blink cleared its lost-context callback. Empty callbacks are now passed through without binding them.
- Context disposal then waited for ANGLE post-link tasks in an unstarted desktop thread pool. The QEMU GDB stack (`gpu-teardown.gdb.log`; local artifact) from the pre-fix failing build (`gpu-teardown-debug-suite.json`; local artifact) identifies `ProgramExecutableVk::waitForPostLinkTasksImpl`. Firmware now uses ANGLE's synchronous task pool.
- SwiftShader's default raster batch pool exceeded the tracked heap when devices overlapped. The firmware pool has two batches. Dawn service teardown also releases native resources immediately on disconnect, rather than retaining devices until surviving DOM handles are garbage-collected.

## Validation on 2026-10-20

The final EFI image SHA-256 is `865284006e9c7808c06b5b9954047e4659cf198a13f6c9f8e682c30dfecc55dc`. QEMU used KVM, OVMF, 4 GiB RAM and isolated raw FAT profiles. Pages were loaded over HTTPS through address-bar input. Graphics checks inspect both JavaScript readbacks and framebuffer pixels captured from QEMU.

| Scenario | Assertions | Result |
| --- | --- | --- |
| WebGL 1 | Context creation, clear/readPixels, GLSL compile/link, triangle rasterization, no rendering errors and invalid-command validation. | 6 checks and visible red triangle on green canvas; 1 and 4 CPUs passed. |
| WebGL 2 | WebGL rendering checks plus GLSL ES 3.00 and buffer copy/readback. | 8 checks and framebuffer pixels; 1 and 4 CPUs passed. |
| WebGPU | Adapter/device, 64-element WGSL compute, storage binding bounds, compiler diagnostics, vertex/fragment rasterization, native canvas snapshot and opaque alpha, validation errors, device recreation and loss notification. | 13 checks and framebuffer pixels; 1 and 4 CPUs passed. |
| WebGPU canvas | RGBA8/BGRA8/RGBA16Float, opaque/premultiplied alpha, resize, aligned edge pixels, unconfigure, invalid-state rejection and retained front buffer. | 11 checks and blue framebuffer pixels; 1 and 4 CPUs passed. |
| GPU lifecycle | Alternate WebGPU and WebGL 2 through three navigation cycles, repeating execution checks and screenshot assertions. | 6 navigations passed on 4 CPUs; no renderer restart or assertion. |
| Dedicated workers | Existing worker integration scenario. | Passed on 4 CPUs. |
| Service probes | IndexedDB transaction, WebGL readPixels, WebGPU adapter/device, VP8 decoded pixel, PCM WAV decode and worker round trip. | All 6 probes reported working on 4 CPUs. |

The storage-binding test writes beyond a 16-byte GPU buffer binding and verifies that the remaining bytes in the underlying buffer are unchanged. This is one shader-boundary regression, not proof of a renderer or driver security sandbox.

Results: 4-CPU suite (`gpu-complete-suite.json`; local artifact), 1-CPU suite (`gpu-complete-single-suite.json`; local artifact), service details (`gpu-complete-service-capabilities-service-capabilities-results.json`; local artifact), native WebGPU screenshot (`gpu-complete-webgpu-input-webgpu.png`; local artifact), WebGL 2 screenshot (`gpu-complete-webgl2-input-webgl2.png`; local artifact). The build, final EFI link and binary validation (`gpu-binary.log`; local artifact) passed. The binary check finds no required unresolved symbols, hosted imports or host syscall instructions; optional weak hooks are listed in its log.

Reproduction commands, run from the project directory with the fixture server kept running:

```sh
python3 build-blink-libraries.py --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 tests/network-servers.py
```

In another terminal:

```sh
python3 tests/state-suite.py --workers 2 --https-ca build/network-https/ca.esl --prefix gpu-complete webgpu webgl webgl2 webgpu-canvas gpu-lifecycle workers service-capabilities
python3 tests/state-suite.py --cpus 1 --workers 2 --https-ca build/network-https/ca.esl --prefix gpu-complete-single webgpu webgl webgl2 webgpu-canvas
```

Python and fixture JavaScript syntax checks passed. All 65 Chromium patch sections and the three graphics dependency patches passed reverse-application checks against the build checkout. Graphics adapter sources match their compiled source mirror. Full WebGL/WebGPU CTS, WPT, physical hardware and hostile-renderer sandbox tests were not run. Human review and independent validation remain required for these AI-assisted changes.
