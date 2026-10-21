# CPL3 renderer boundary

The default browser build enters the real Blink/V8 payload at CPL3. DOM, JavaScript, layout, Skia, workers, ANGLE, Dawn and SwiftShader execute in that address space. The browser UI and firmware services remain on the BSP. This boundary confines renderer-native memory accesses; it does not implement Chromium site isolation or establish that the trusted service implementations have no vulnerabilities.

## Enforcement

| Boundary | Implementation |
| --- | --- |
| Privilege entry | Private GDT user code/data descriptors, TSS `RSP0`, and `iretq` into the ELF entry. User flags have IOPL 0 and interrupts disabled. The TSS has no accessible I/O bitmap, so port I/O is denied. |
| Address translation | The loader clones page-table paths and removes inherited user access before granting individual pages. The page tables and their physical identity aliases remain supervisor-only. The sparse renderer arena has an independent PML4 subtree. Four-level paging, CR0.WP and EFER.NXE are required. |
| Initial mappings | Immutable ELF segments are read-only; writable image data has private backing. Stack and TLS are writable and NX. A read-only bootstrap contains the application-channel address without the real system-table or image handle. |
| Allocation | Pool requests use dedicated, zeroed firmware pages in CPL3 mode. Loader-owned grants record the physical range and maximum write permission. Firmware pool metadata and adjacent allocations are not exposed. |
| Mapping operations | The kernel accepts mappings only inside the renderer arena and only to granted backing. Protection changes cannot modify supervisor or immutable ELF pages. Freeing a grant removes its identity user permission and all sparse-arena aliases before firmware reuses the pages. The AP reloads CR3 after mapping changes and broker returns. |
| System calls | `int 0x80` enters a supervisor-only stack, saves registers, clears DF and dispatches a fixed numeric operation. The entry does not call a supplied function pointer or use a user stack as a kernel stack. The loader dispatcher uses no TLS, allocator, SIMD or payload callbacks. |
| Firmware services | The supervisor copies five request arguments to a protected channel and waits for the BSP broker. Existing ownership, range, nested-copy, descriptor, variable and format checks remain in force. The legacy BSP `mprotect` operation is denied in CPL3 mode; it cannot change the BSP page tables. |
| TLS | A fixed operation reads or changes FS_BASE. A replacement must cover a writable user range. Cooperative thread switching uses the gate instead of `rdmsr`/`wrmsr`. |
| HTTP response ownership | Response bodies, header arrays and header strings are copied into dedicated read-only user mappings. The BSP retains the original response and the authoritative release record. |
| Presentation | A dedicated shared page allocation contains command slots and frame pixels. The BSP uses private dimensions and pixel addresses, bounds strings and validates preferences. CPL3 mode never installs the renderer's `ScrollScene*` or dereferences its C++ object graph. |
| Faults and termination | Exceptions and NMI use separate supervisor IST stacks. Fault records include vector, error, RIP, CS and CR2. Recovery restores firmware state, waits for AP completion, releases grants and starts a new renderer. Invalid user RSP does not supply the exception stack. |
| Startup | A missing AP or unsupported CPU/firmware configuration fails closed. The default build does not substitute a privileged local renderer. |

The numeric ABI is defined in `runtime/renderer_sandbox.h`. Its operations are ABI query, validated TLS change, user-page permission query, exit, owned-page mapping, owned-range protection, firmware broker dispatch and nested JIT write scopes. Unknown operations return `-ENOSYS`; invalid broker argument arrays return `-EFAULT`.

The implementation follows the paging, privilege, TSS, interrupt-gate and interrupt-return mechanisms described in the [Intel software developer manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html), volume 3A. It uses an interrupt gate rather than the `syscall` instruction; the EFI payload still has no dependency on a host OS syscall ABI.

## Changed implementation files

- `src/loader-sandbox.h`, `src/loader-renderer.h`, `src/loader.c`, `runtime/platform/jump.S`: protected mappings, descriptors, entry, syscall dispatch and exception recovery.
- `runtime/renderer_sandbox.h`, `runtime/renderer_runtime.h`, `src/process/renderer_runtime.c`, `src/virtual_memory.c`, `src/posix.c`, `src/musl/thread-switch.S`, `src/musl/blink_compatibility.c`: syscall ABI, page-backed broker allocation, user mapping metadata, protection queries and TLS/atomic compatibility.
- `src/network/async.c`, `src/blink/remote_frame.cc`: read-only response exports, bounded presentation data and trusted channel reconstruction after a renderer fault.
- `src/blink/browser_entry.cc`, `src/blink/browser_settings.h`, `build-musl.sh`, `README.md`: default CPL3 startup, cleanup on refusal and accurate UI/build documentation.
- `src/process/cpl3_probe.h`, `src/musl/probe.c`, `tests/cpl3-startup.py`, `tests/state-suite.py`, `tests/browser-qemu.py`, `tests/input-scenario.py`: startup boundary probes, live fault injection, privilege assertions and startup refusal verification.

## Build and validation commands

```sh
python3 build-blink-libraries.py --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py

# Run these fixture servers in separate terminals.
python3 tests/network-servers.py
python3 tests/websocket-server.py

python3 tests/state-suite.py --workers 2 --require-cpl3 \
  --https-ca build/network-https/ca.esl --prefix cpl3-production \
  network-secure websocket workers webgl webgpu indexeddb cache audio heavy-site

# This image includes destructive test entry points and is only for QEMU testing.
ENABLE_BLINK=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/cpl3-probe" ./build-musl.sh
python3 tests/state-suite.py --workers 1 --build-directory build/cpl3-probe \
  --require-cpl3-probe --https-ca build/network-https/ca.esl \
  --prefix cpl3-release-isolation sandbox
```

`./run-qemu.sh` opens the normal image in SDL. `ENABLE_RENDERER_CPL3=0` explicitly selects the legacy unconfined implementation. It is not a sandbox option for production use.

## Scope and remaining work

- Tabs and workers share one renderer. A compromised renderer can access its other documents and the profile services granted to that renderer. There is no per-origin native process or file-service isolation.
- V8 JIT mappings now use supervisor-owned write scopes and alias-wide write/execute exclusion. Executable pages are RX outside a scope and writable NX inside it. Physical backing stays NX and is read-only while its code can execute. See [JIT implementation and newer QEMU evidence](JIT-WX.md). Native renderer code can request legitimate scope transitions; this is not compiler authentication or control-flow integrity. Static ELF code remains read-only and initial stacks remain NX.
- Cached asynchronous scrolling now uses a bounded pixel-tile wire protocol. See [scroll transport verification](SCROLL-WIRE.md) for the newer implementation and its separate image hashes. DOM event handlers and uncached content still wait for renderer work.
- Page-table storage is bounded to 2,048 pages per renderer. Empty tables are reclaimed when the renderer exits, not during its lifetime. Broker allocations retain the existing 2 GiB/16,384-entry limit.
- Firmware, BSP parsers, broker implementations, DMA devices and the CPU remain trusted. No IOMMU policy, speculative side-channel defense, SMEP/SMAP policy, per-site isolation, independent security audit or physical-hardware validation is claimed.
- The negative tests check explicit forbidden accesses and recovery. They are not an exhaustive native-code attack campaign or proof of absence of broker vulnerabilities.

## Recorded results

Validation ran on QEMU 11.1.2 (Debian `1:11.1.2+ds-3`) with KVM, OVMF `2026.08+ds-2`, 4 GiB RAM and four virtual CPUs. The compiler was Debian Clang 23.1.3. The startup refusal test used one virtual CPU.

| Image and record | Result |
| --- | --- |
| Final normal image, [release suite](cpl3-release-suite.json) | HTTPS network checks, `zeroday0619.dev` loading and native mouse/keyboard/wheel interaction, and six renderer lifetimes during NMI/page-fault/invalid-opcode/breakpoint recovery passed. Every reported Blink/V8 event loop had CPL 3. |
| Final normal image, [single-CPU refusal](cpl3-single-cpu.json) | Startup refused privileged fallback, created no web frame and completed engine cleanup. The expected startup failure is recorded in the log. |
| Final normal image, [binary validation](cpl3-release-binary.log) | Static EFI/ELF checks passed. Optional weak hooks are listed separately from required symbols. |
| Final probe image, [isolation suite](cpl3-release-isolation-suite.json) | All 11 startup cases passed. A live website renderer then corrupted its shared channel header and attempted a supervisor read: `#PF error=5`, `CS=0x5b`. A second live renderer attempted to write its root page table: `#PF error=7`, `CS=0x5b`. Both recovered and accepted a page-button click. |
| Earlier integrated CPL3 image, [nine-case regression](cpl3-production-suite.json) | HTTPS, WebSocket/WSS, workers, WebGL, WebGPU, IndexedDB, Cache Storage, AC'97 output and the real website passed. The final normal image subsequently added startup cleanup, shared-channel reconstruction, inherited U/S-bit clearing and user breakpoint-gate handling; its narrower rerun is listed above. |

Image SHA-256 values:

```text
Final normal: c234f135ca9d6faa10fcdd52b3ff313156a0752fe8e334872a88f8c7fe89578a
Final probe:  ec30849fcf9f8dceeb798dc2dd67c9d336271170f43a39545ef79a31f90899e4
Earlier nine-case image: b300aef4cf6f8053553a62303f6f5779c714e07097ed0e08ce746ff1491b33a6
```

The 11 startup cases cover successful user entry and syscall return; supervisor reads and writes; page-table writes; static code writes; NX stack execution; CR3 reads; port I/O; an invalid user stack; CLI; and the full C runtime in CPL3. Each entry also rejects unknown system calls, invalid TLS addresses, a supervisor page supplied as mapping backing, an out-of-arena mapping, supervisor protection changes and a supervisor pointer supplied as the broker argument array. The full browser runtime reports 71 checks, including sparse virtual memory, cooperative TLS/FP switching and shared aliases.

The [live isolation runner](cpl3-release-isolation-sandbox-runner.log) uses GDB only to select a test entry point after observing CPL3. It does not emulate a successful fault or modify the supervisor memory itself. The injected instructions execute inside the real renderer. The normal image excludes these retained test entry points.

The [real-site screenshot](cpl3-release-heavy-site-input-heavy-loaded.png) shows Korean text, page imagery and browser controls. The [post-fault screenshot](cpl3-release-isolation-sandbox-input-sandbox-recovered-renderer_boundary_attack_table.png) shows the restored test document after the protected page-table write. These screenshots were inspected along with the serial and scenario logs; no pixel-perfect compatibility claim is made.

Python compilation checks for the modified QEMU runners, `bash -n build-musl.sh` and source `git diff --check` passed. No dependencies were added and no files were staged or committed.
