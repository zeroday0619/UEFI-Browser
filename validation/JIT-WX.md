# Renderer JIT write and execute separation

## Implementation

The supervisor classifies executable arena mappings as JIT-controlled pages. It records requested write and execute permissions in protected page-table software bits. The actual leaf permissions never enable write and execute simultaneously: code is RX outside a write scope and RW/NX inside one. Read-only aliases retain their write restriction. The physical identity alias remains NX and becomes read-only while code can execute.

Registration also revisits preexisting arena aliases of the same backing page. Repeated map requests cannot restore writable executable access. Direct protection changes cannot make identity mappings executable or restore write access to executable backing outside a scope. Removing the final alias restores writable NX backing for reuse. Grant revocation removes all arena aliases before returning memory to firmware.

`RENDERER_SYSTEM_JIT_SCOPE` enters or leaves a nested scope. Scope depths are keyed by the validated TLS base in supervisor-owned storage. The TLS-switch operation restores the destination thread's mode and reloads CR3. This is necessary because firmware workers are cooperatively scheduled on the same renderer processor. It does not provide a design for concurrent execution of this address space on several processors.

The UEFI branch of V8's `RwxMemoryWriteScope` calls the fixed scope operation. Host generators retain their original platform behavior. The source patch is `patches/blink-v8-jit.patch`; `prepare-blink.py` applies it independently of the existing V8 port patch.

Outer scope transitions visit only page-table branches marked as containing JIT mappings. Registration still examines existing aliases before a page becomes executable. New mappings mark every ancestor, including mappings added to an existing data-only branch. Ancestor marks are conservative and may remain after unmapping until renderer teardown. The host test makes an unmarked data branch inaccessible with `mprotect` to verify that transitions do not inspect it, then adds code to that branch and verifies its transitions.

The default `build/blink` image now includes this implementation. The opt-out privileged renderer does not gain this protection. Build invocations use a host `flock` lock because separate EFI output directories still share one musl build directory.

## Executable-memory discard

The first complete DevTools run reached Console evaluation and debugger pause, step and resume, then faulted during reopening. `memset` attempted to write read-only physical backing. The existing `virtual_memory_discard` implementation zeroed backing directly for `MADV_DONTNEED`, bypassing the virtual mapping's intended protection.

A runtime probe reproduced the same CPL3 page fault with error 7 when discarding an executable mapping. `jit-wx-discard-repro-test.log` and `jit-wx-discard-repro-serial.log` retain the failure. Protected backing is now replaced with a fresh zeroed page; remapping preserves requested permissions and releases the old backing only after the mapping changes. Writable backing retains the direct zeroing path. No JIT protection is disabled for discard.

The probe repeats code generation, execution, discard, zero-content and RX-permission checks, unmapping and backing reuse eight times. The corrected full-browser run also completes DevTools reopening and target detachment without a renderer fault.

The initial Console test separately lost its first character because it typed before the lazy editor was ready. The harness now waits for the rendered prompt and focuses it before injecting keyboard events. It still verifies the resulting DOM value and computed CSS color on the inspected page.

## Boundary verification

The host test includes the production supervisor policy header. AddressSanitizer and UndefinedBehaviorSanitizer passed alias permission transitions, nested scopes, thread-mode restoration, backing reuse, scope-slot exhaustion and depth overflow.

```sh
clang -O1 -g -fsanitize=address,undefined tests/jit-policy-host.c \
  -o build/jit-policy/host
build/jit-policy/host
```

A separate runtime image executes actual user-mode machine code and deliberately faults through the same loader, page tables, interrupt gate and exception recovery used by the browser:

```sh
ENABLE_RENDERER_CPL3=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/jit-boundary" ./build-musl.sh
python3 tests/jit-qemu.py
```

`jit-wx-boundary.json` records 14 passing CPL3 cases for image SHA-256 `55f0d04f3c51d43a5f3e628e7887a2e650ae3fd1c64bc1a07e63008c15c8a69d`. The non-destructive case writes a return-value function through an alias, checks kernel-reported permissions, tests nested scopes and remapping, executes the function from another cooperative thread, restores the original write scope, executes after closing it and reuses the backing after unmapping. It also covers executable-memory discard.

| Attempt | Observed fault |
| --- | --- |
| Write the executable arena page outside a scope | Page fault, error 7, CS `0x5b` |
| Execute the writable arena page inside a scope | Page fault, error 21, CS `0x5b` |
| Write the physical identity alias of executable code | Page fault, error 7, CS `0x5b` |

`jit-wx-boundary.log` retains serial evidence and `jit-wx-pruned-policy.log` retains the host-policy result. The updated general CPL3 probe expects 14 cases; older 11-case artifacts remain historical evidence.

## Browser verification

The fixture server is `python3 tests/network-servers.py --directory build/jit-wx-discard-https`. It is stopped after the suite.

```sh
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/jit-wx" ./build-musl.sh
python3 tests/state-suite.py --cpus 4 --workers 2 \
  --build-directory build/jit-wx \
  --https-ca build/jit-wx-discard-https/ca.esl --prefix jit-wx-fixed \
  --require-cpl3 --require-no-renderer-faults \
  devtools workers webgl webgpu heavy-site renderer-scroll
ENABLE_BLINK=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/jit-wx-probe" ./build-musl.sh
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --build-directory build/jit-wx-probe \
  --https-ca build/jit-wx-discard-https/ca.esl --prefix jit-wx-isolation \
  --require-cpl3-probe sandbox
```

All six normal scenarios passed at CPL3 with no unexpected renderer fault. `jit-wx-fixed-suite.json` records the image hash and individual results. DevTools exercised Console DOM/CSS mutation, pause/step/resume, reopening and detachment. Its stepped Sources screenshot shows line 25 and the matching call frame. The real-site case exercised `zeroday0619.dev`. The six-second JavaScript scroll case observed visible scrolling after 16.41 ms, unchanged fixed controls and zero reconciliation difference; this is one fixture measurement, not a general latency guarantee.

`jit-wx-isolation-suite.json` records all 14 startup probes in the full browser image and two live forbidden accesses. Supervisor reads and root-page-table writes faulted, each renderer recovered, and the page accepted a subsequent button click. Intentional faults are expected in this scenario.

The default image was rebuilt with `ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh`. `jit-wx-release-suite.json` records successful DevTools, Korean and Japanese input scenarios with CPL3 execution and no unexpected renderer fault. The host fixture for this run used `build/jit-wx-release-https`; the suite used `--prefix jit-wx-release --require-cpl3 --require-no-renderer-faults devtools ime ime-japanese`. `jit-wx-release-binary.log` records successful static EFI/ELF checks. The final EFI SHA-256 is `330491555f013be006366a3b35d2048101ba80478ad334a095b3c9d5a0cf8255`. Its payload is byte-identical to the six-scenario integration image. `jit-wx-release-sources.json` records selected implementation, test and binary hashes.

## Remaining security scope

These tests establish the exercised page-permission transitions and faults, not a complete sandbox audit. Native renderer code can request legitimate write-scope transitions; this mechanism does not authenticate a JIT compiler or establish control-flow integrity. Per-site processes, service separation, DMA policy, side-channel defenses, concurrent address-space execution, comprehensive broker validation and independent review remain separate requirements.
