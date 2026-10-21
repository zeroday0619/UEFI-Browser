# Dynamic C heap and fatal runtime display

## Allocation behavior

The musl allocator obtains firmware backing on demand. Its requested-byte budget is now one quarter of available firmware pages at process initialization, capped at 1 GiB. BSP and renderer compute separate budgets. If the memory query fails, the existing 128 MiB fallback applies. This budget is not a physical-memory reservation or a promise that every request below it will succeed.

The renderer continues to obtain backing through the CPL3 service broker. Small allocations use seven slab size classes. Each slab owns its free list and live-block count; releasing its last block returns the complete slab to the broker and firmware. This avoids retaining every slab allocated during a temporary burst. The broker charges the actual rounded backing pages against its separate 2 GiB aggregate limit. These physical charges include both renderer pool and page requests.

Allocation and alignment arithmetic reject overflow. Allocation failures set `ENOMEM`; invalid alignment sets `EINVAL`. `posix_memalign` leaves the output pointer and `errno` unchanged on failure. `free` preserves `errno`. Shrinking or regrowing within an existing raw allocation changes accounting without a second allocation; failed growth preserves the old pointer and contents. Requested bytes, backing-request bytes and the process budget are exposed separately. Backing-request bytes exclude firmware rounding, sparse virtual commitments and worker pages.

The tab admission check and scroll decoder now use the same process budget instead of assuming 128 MiB. The process monitor identifies its figures as the BSP heap, not total browser or renderer memory. The old non-musl prototype allocator in `src/runtime.c` is not used by this browser build and retains its original limit.

## Fatal display

A privileged musl `abort()` now clears the UEFI text console and displays a fatal-error header, the caller address, current RSP/CR3/CR2/RFLAGS and the last twelve diagnostic lines. Log capture uses a fixed 4 KiB ring. The formatting path uses fixed stack buffers and no C heap, stdio locks or Skia rendering. It still depends on firmware console services; those services may allocate internally. The processor halts after rendering. Recursive calls skip rendering and halt.

Renderer `abort()` still traps at CPL3 and uses the existing supervisor recovery path. The new display does not turn recoverable document faults into system-wide halts. Arbitrary BSP hardware exceptions and loader failures are not yet connected. The displayed registers describe panic entry, not a saved hardware-exception frame.

## Changed sources

- `src/musl/allocator.c`, `runtime/platform/runtime.h`: dynamic budget, slab ownership/reclamation, allocation arithmetic and statistics.
- `src/process/renderer_runtime.c`: rounded physical backing quota charges.
- `src/blink/browser_entry.cc`, `src/blink/scroll_wire.cc`: shared budget use and accurate BSP memory display.
- `src/musl/startup.c`: fixed diagnostic ring and privileged fatal screen.
- `src/musl/memory_probe.c`, `src/musl/probe.c`, `build-musl.sh`: opt-in real-runtime allocation and fatal probes.
- `tests/allocator-host.c`, `tests/allocator-host.py`: production allocator with fault-injected firmware under ASan/UBSan.
- `tests/panic-qemu.py`, `tests/state-suite.py`: captured fatal-screen and CPL3 allocation assertions.

## Commands

```sh
python3 tests/allocator-host.py
python3 build-blink-libraries.py --jobs 20
ENABLE_PANIC_PROBE=1 BUILD_DIRECTORY="$PWD/build/panic-probe" ./build-musl.sh
ENABLE_BLINK=1 ENABLE_MEMORY_PROBE=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/memory-probe" ./build-musl.sh
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
python3 tests/panic-qemu.py
```

Keep `python3 tests/network-servers.py` running while executing these suites so the generated test CA remains valid:

```sh
python3 tests/state-suite.py --workers 2 --build-directory build/memory-probe \
  --require-cpl3-probe --require-memory-probe --require-scroll-wire \
  --https-ca build/network-https/ca.esl --prefix dynamic-memory-final-probe \
  renderer-scroll sandbox tabs-site
python3 tests/state-suite.py --workers 1 --build-directory build/blink \
  --require-scroll-wire --https-ca build/network-https/ca.esl \
  --prefix dynamic-memory-final scroll-input heavy-site
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 -m py_compile tests/allocator-host.py tests/panic-qemu.py tests/state-suite.py
git diff --check -- . ':!validation/*.log'
```

## Evidence

The host sanitizer test passes allocation, alignment, overflow, failure preservation, errno, in-place shrink/regrowth, all slab classes, partial/free-list reuse and complete reclamation checks. It is a host test with mocked firmware, not proof of firmware execution.

The EFI memory probe allocates 160 MiB, writes and checks every 4 KiB page plus the last byte, shrinks/regrows in place, verifies failed growth, frees the block and checks accounting restoration. It also allocates and frees 1,300 small blocks and checks backing restoration. Suite acceptance requires the probe markers at both CPL0 and CPL3.

The final memory/runtime changes before adding POST passed three probe scenarios and two normal browser scenarios. The later POST image repeats startup, scroll, live-site and boundary checks; see [BOOT-POST.md](BOOT-POST.md). The panic probe runs the same fatal-display source in a minimal musl image, without Blink stack collection.

| Evidence | Result |
| --- | --- |
| [Host sanitizer log](dynamic-memory-host.log) | Passed with ASan and UBSan. |
| [Memory/boundary suite](dynamic-memory-final-probe-suite.json) | 3/3 pass: root scroll, live sandbox faults/recovery and real-site tabs. Every case records 160 MiB allocation/reclamation at CPL0 and CPL3 and all 11 startup boundary cases. |
| [Normal-image regression](dynamic-memory-final-suite.json) | 2/2 pass: cancelable/passive wheel, keyboard, drag controls and real-site interaction. |
| [Fatal-console test](panic-qemu.json) | Intentional BSP abort produces a stable 1280×800 screen. [Visual inspection](panic-qemu.png) confirms the fatal header, caller/register values and recent diagnostics. |
| [Binary validation](dynamic-memory-binary.log) | Passed static EFI/ELF and required-symbol/instruction checks. |

The 4 GiB QEMU memory probe measured a BSP requested-byte limit of 916,656,128 bytes and an initial CPL3 limit of 911,501,312 bytes. Limits depend on available memory at initialization and can differ after recovery. The normal input regression observed the first keyboard-scroll pixel change after 20.83 ms during a 12-second JavaScript task. The probe root-scroll test observed 21.14 ms during a 6-second task. Both reconciliation checks found no channel differences greater than 2. These are QMP observations, not physical display latency guarantees.

Pre-POST normal image SHA-256: `24407ad488f5b2de2a052cad38d5358151b6d33455b8ed4e4b12cb4737c2805a`.

Pre-POST memory/boundary image SHA-256: `26bb76b9a75701455769b57574036a6070572ca64493ef6376b03cade478adf9`.

Panic image SHA-256: `0b37a9c3bd28687c444f9a81d1c0d10bb6c705cce25d4fd358453d079c741222`.

Python syntax and source diff-whitespace checks passed. Raw generated build logs are excluded from whitespace checking. No full Chromium conformance or physical-hardware tests were performed. The full objective in `IMPLEMENTATION-GOALS.md` remains incomplete.

## Subsequent page reclamation and table growth

[PAGE-RECLAMATION.md](PAGE-RECLAMATION.md) records empty renderer batch reclamation, a fixed supervisor page-table exhaustion reproduction, bounded BSP table growth and the remaining live-site data-quota failure. The C heap limit, physical broker quota and supervisor table reserve are distinct limits. Passing the sparse mapping probe does not establish sustained YouTube playback.

The subsequent [grant revocation optimization](GRANT-REVOCATION.md) identifies a BSP page-table walk during ordinary heap release and records a controlled comparison plus sandbox and browser regressions.

[DYNAMIC-REGIONS.md](DYNAMIC-REGIONS.md) records removal of the fixed virtual
reservation-slot limit, failure-safe registry growth and its validation boundary.

[BROKER-ALLOCATION-INDEX.md](BROKER-ALLOCATION-INDEX.md) records indexed trusted
allocation lookup, retained byte/page permission checks and a controlled QEMU
comparison of buffer-validation cost.
