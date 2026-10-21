# Isolated presentation worker processes

Implemented and tested on 2026-10-18. This is the process-runtime and framebuffer-presentation stage. Blink DOM, JavaScript, layout, Skia rasterization and networking still execute in the main process. The requested complete browser/renderer process split is not complete.

## Implementation

`src/process/process.c`, `process.h` and `entry.S` implement up to four worker processes on UEFI application processors. Each worker has a distinct CR3, private user stack and state, read-only executable code, a read-only command page and a private supervisor stack/TSS. Existing firmware mappings are supervisor-only in the child root. The worker executes at CPL3 and returns through a private interrupt gate. A shared source buffer is read-only; an explicitly shared destination contains the framebuffer. This is a fixed worker program, not a general executable loader or POSIX fork implementation.

The supervisor dispatches work through acquire/release mailboxes. Workers copy disjoint rows, convert RGB/BGR when needed and use streaming stores on aligned rows. Allocation, firmware calls other than the explicitly permitted MP WhoAmI service, network activity and Blink remain on the BSP. Worker exceptions return to the supervisor; failed presentation disables the parallel path and uses GOP. An unresponsive worker causes fail-stop rather than freeing memory it could still execute.

Persistent AP services avoid the approximately 10 ms processor restart cost on every frame. The main loop stops idle workers after 250 ms without a frame. After idle, presentation immediately falls back to BSP output. Completion of AP shutdown is polled once per input-loop pass. Four presentations separated by less than 100 ms request AP restart from the input loop, outside the cursor lock. Startup itself still uses the firmware MP service and can delay that loop; this is not a general asynchronous process scheduler. While active, idle services use PAUSE polling. A long main-thread task can delay idle shutdown. No general thread scheduler, timer preemption, worker-process restart supervisor or Chrome renderer sandbox is implemented.

A null task register cannot be restored with LTR. At final teardown, MP timeout termination resets the used APs. A verification callback checks the task register, CR3, GDT and IDT before freeing temporary process mappings and descriptors. This path was tested with the pinned OVMF. Nonblocking MP support is required for persistent services; initialization failure retains BSP presentation where safe.

`browser_entry.cc` invokes worker presentation for directly mapped RGB/BGR GOP modes and retains ordinary GOP blits for unsupported cases. `build-musl.sh` compiles and links the process runtime. `prepare-blink.py` copies its public header. The runner now defaults to four virtual CPUs; `QEMU_CPUS=1` selects the single-CPU fallback. The About page explicitly identifies Blink as remaining in the main process.

## Validation

QEMU 11.1.2, OVMF 2026.08 and 4 GiB guest RAM were used. The final EFI hash is recorded in `process-result.json` (local artifact). No new production dependency was added.

- KVM with 1, 2 and 4 CPUs: byte-identical settings framebuffer, actual device input, clean browser teardown and return to firmware.
- Separate CR3 values and different private state at the same user virtual address.
- A CPL3 barrier requiring every assigned AP to run concurrently; three workers passed on four virtual CPUs.
- An attempted write to supervisor memory faults with a user/write/protection page-fault error; parent data remains unchanged and later worker jobs run.
- RGB/BGR conversion, odd widths, uneven row partitions, padded strides and untouched guard pixels.
- Idle stop/restart and CPU descriptor retirement before resource release.
- Four-CPU settings controls, search, theme changes, startup choice, About and back navigation.
- Two-CPU TCG execution and teardown.
- Four-CPU KVM loading of `https://zeroday0619.dev`, loading-time input, post-load theme click, Space activation and wheel scrolling.
- Static ELF/EFI checks: no required unresolved symbols, Linux syscall instructions or PE imports. The worker's INT 0x80 is a local supervisor-return gate, not a Linux system call.

Commands from the workspace root:

```sh
python3 outputs/chromium-uefi/build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/outputs/chromium-uefi/build/blink" outputs/chromium-uefi/build-musl.sh
python3 outputs/chromium-uefi/tests/process-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=settings python3 outputs/chromium-uefi/tests/browser-qemu.py
QEMU_TEST_ACCEL=tcg QEMU_TEST_CPUS=2 INPUT_SCENARIO=settings-preview python3 outputs/chromium-uefi/tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=heavy-site python3 outputs/chromium-uefi/tests/browser-qemu.py
BUILD_DIRECTORY="$PWD/outputs/chromium-uefi/build/blink" python3 outputs/chromium-uefi/validate-binary.py
QEMU_CPUS=4 outputs/chromium-uefi/run-qemu.sh
```

Logs: 1 CPU (`process-1cpu.log`; local artifact), 2 CPUs (`process-2cpu.log`; local artifact), 4 CPUs (`process-4cpu.log`; local artifact), settings input (`process-input.log`; local artifact), TCG (`process-tcg.log`; local artifact), real website (`process-site.log`; local artifact), binary validation (`process-binary.log`; local artifact).

## Performance and remaining work

The original short settings comparison measured four presentations per run. Total presentation time was 2,307 microseconds with one CPU, 14,539 with two CPUs and 13,155 with four CPUs. The parallel runs include worker restart after idle. This workload is slower with the new process path; these results do not establish a browser speedup. The comparison checks output correctness and lifecycle behavior, not statistically stable throughput or optical latency. Current idle-resume measurements and limitations are in [IDLE-RESUME.md](IDLE-RESUME.md).

The next architectural boundary is moving Blink/V8 and Skia rasterization into a renderer process. That requires a user-mode C/C++ runtime, per-process allocation and TLS, brokered firmware/network operations, input/frame IPC and renderer failure recovery. The current privileged runtime still rejects pthread_create and does not implement fork/exec. Application processors currently parallelize completed-frame transfer only. Physical firmware, arbitrary child programs, malicious-code sandboxing, asynchronous external interrupts inside the child supervisor and performance across other GOP devices remain unverified.

Protocol reference: the pinned EDK II [MP Services declaration](https://github.com/tianocore/edk2/blob/2970e5699ba6267f3384ffab20f96647578aebc8/MdePkg/Include/Protocol/MpService.h) specifies BSP-only dispatch, allowed AP operations, nonblocking events and timeout termination. The corresponding [MP initialization implementation](https://github.com/tianocore/edk2/blob/2970e5699ba6267f3384ffab20f96647578aebc8/UefiCpuPkg/Library/MpInitLib/MpLib.c) supplies the tested AP reset behavior.
