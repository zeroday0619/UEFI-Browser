# UEFI Virtio-GPU transport

## Implemented boundary

`src/gpu/virtio-probe.c` is a standalone UEFI application that takes ownership
of one modern Virtio GPU. It negotiates VERSION_1, VIRGL, RESOURCE_BLOB and
CONTEXT_INIT, configures a split control queue and queries display information
and Venus capabilities. It creates and destroys a Venus context three times
using the same identifier. Each iteration creates a 4 KiB guest-backed blob,
attaches and detaches it from that context, and releases the resource.

Every submitted command requests a fence. Completion requires a bounded used
length, the expected response type, the fence flag and the exact 64-bit fence
identifier. The 16-bit submission counter is converted before selecting its
used-ring slot; subtracting one as a promoted signed integer would otherwise
select a negative slot when the counter reaches zero. The probe submits 65,536
extra display requests to exercise that transition on the actual device.
Reset acknowledgement precedes release of queue and blob DMA backing.

The application uses its own PCI and Virtio transport rather than the firmware
GPU driver's queues. It halts after cleanup and must not be started inside a
browser that continues to use that firmware graphics device. This application
is separate from the browser build and does not replace the default EFI image.

## QEMU validation

The tested EFI SHA-256 is
`ecbc9695620972aff292f1cbbb82b6a422bbeb9b2e23ba6d2114c07d163d4510`.
`virtio-gpu-context-index-wrap.json` records 65,559 validated fences, the full
16-bit index transition, three context/blob lifecycles and reset/DMA cleanup.
QEMU exited with status zero and no stderr. The positive test selects the
NVIDIA Vulkan ICD and uses the host render node through an EGL-headless Venus
device. `virtio-gpu-context-unsupported.json` verifies that a plain Virtio GPU
is rejected before DMA allocation and is reset successfully.

```sh
bash tools/build-virtio-gpu-probe.sh
python3 tests/virtio-gpu-qemu.py   --icd /etc/vulkan/icd.d/nvidia_icd.json --prefix venus-new-run
python3 tests/virtio-gpu-qemu.py --backend software --prefix no-venus-new-run
python3 -m py_compile tests/virtio-gpu-qemu.py tools/check-venus-host.py
bash -n tools/build-virtio-gpu-probe.sh
```

Each test prefix must be unused so an earlier completion marker cannot satisfy
a new run. The harness also rejects unsuccessful QEMU shutdown.
Earlier `virtio-gpu-context-fence` and `virtio-gpu-guest-blob` reports refer to
intermediate images and are retained separately.

## Official Venus command encoder

The approved Mesa dependency is pinned to 26.2.5, commit
`ddaf85e353442d9d7f192fdcf1823e652e3f2ee3`. `prepare-venus.py` verifies the release
archive SHA-256 before extracting it. The generated protocol identifies
venus-protocol `70991d4c`. Its original headers remain unmodified; the adapter
is in `src/gpu/venus/`. MIT and Apache-2.0 texts are recorded in `licenses/`
and file-level notices remain in the pinned archive.

The opt-in `ENABLE_VENUS_COMMANDS=1` build uses those encoders and decoders
without a guest OS or Vulkan loader. It parses the 64-bit host-visible PCI
capability, creates a HOST3D mappable blob with blob identifier zero, attaches
it to the Venus context and maps it into the PCI aperture. Vulkan replies are
copied to private storage before decoding. Buffer limits, output array counts
and object types are checked before use. This is a synchronous transport
adapter and probe, not the complete Mesa Vulkan ICD.

A GUEST blob is insufficient for the render server's reply stream. The first
attempt fails with `invalid res_id 256`; `venus-commands-first.json` preserves
that failure. HOST3D allocation corrects resource registration. Another failure
exposes use of the global fence timeline: it can complete before the separate
render server writes a reply. SUBMIT_3D now selects the Venus context CPU
timeline explicitly. QEMU returns the context and 64-bit fence identifier but
does not echo the INFO_RING_IDX flag or ring index. The adapter checks the
returned context and exact globally unique fence identifier. One command is
in flight at a time. QEMU 11.1.2 `virtio_gpu_ctrl_response` and
`virgl_write_context_fence` were inspected to verify that behavior.

Graphics queues are obtained with `vkGetDeviceQueue2` and
`VkDeviceQueueTimelineInfoMESA`, as required by virglrenderer 1.3.0.
`vkQueueWaitIdle` is rejected by that renderer. The probe instead submits a
context fence on the queue's GPU timeline and waits for its completion before
reading query results without WAIT_BIT. It creates a device, query pool,
command pool and command buffer, records two GPU timestamps, submits the
buffer, reads the results and destroys the objects. CPU-timeline completion
alone would not establish GPU execution.

`venus-queue-final.json` records the completed QEMU/KVM run with the NVIDIA
ICD. The guest reports NVIDIA GeForce RTX 5080, vendor `0x10de`, device `0x2c02`.
Three context/device lifecycles execute GPU timestamp commands with positive,
ordered results. The test validates 65,643 fences, the 16-bit queue wrap,
resource destruction, reset acknowledgement and DMA backing release. QEMU
exits successfully with empty stderr. The EFI SHA-256 is
`5d8cdac1945fb04b5666882f0e5a3f5e07f9df7b76dd0d4c8cd9656cd3ff9802`.
`venus-sources.json` records the corresponding source and result hashes.

The host adapter test runs under ASan and UBSan and rejects 13 invalid buffer,
count and handle cases. `llvm-readobj` identifies the linked image as an x86-64
EFI application with no import table. The feature-disabled standalone build
also compiles. A plain software Virtio GPU is rejected before DMA allocation
and resets successfully in `venus-queue-unsupported.json`.

```sh
ENABLE_VENUS_COMMANDS=1 BUILD_DIRECTORY="$PWD/build/venus-queue-final" \
  bash tools/build-virtio-gpu-probe.sh
python3 tests/venus-stream-host.py
python3 tests/virtio-gpu-qemu.py \
  --image build/venus-queue-final/esp/EFI/BOOT/BOOTX64.EFI \
  --gpu-queue --icd /etc/vulkan/icd.d/nvidia_icd.json \
  --prefix venus-queue-new-run
python3 tests/virtio-gpu-qemu.py \
  --image build/venus-queue-final/esp/EFI/BOOT/BOOTX64.EFI \
  --backend software --gpu-queue --prefix venus-queue-unsupported-new-run
```

## Remaining integration

[Venus](https://docs.mesa3d.org/drivers/venus.html) serializes Vulkan commands
for a host renderer. This standalone application now sends actual Vulkan
commands and executes timestamp operations on a GPU queue. The subsequent
opt-in rendering path also executes vertex/fragment shaders, renders a triangle,
exports and reads its image, and presents it through an independent GOP
framebuffer. See [hardware rendering validation](VENUS-RENDERING.md) for the
image identity and pixel evidence. General resource handling, Virtio scanout,
asynchronous presentation and browser integration remain unfinished. The
ordinary browser still uses SwiftShader for WebGL/WebGPU. Its default image is
unchanged.

The complete Mesa Vulkan runtime and asynchronous command ring still require
UEFI integration. CPL3 broker validation, DMA isolation, quotas, cancellation
and GPU fault recovery remain required before exposing this transport to web
code. These checks do not establish a complete GPU driver or sandbox.
