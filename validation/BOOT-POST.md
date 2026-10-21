# Browser loader POST

`src/loader-post.h` displays application startup checks before the browser runtime starts. It does not replace the machine firmware's POST or diagnose all installed hardware.

The loader verifies CPU long-mode, SSE2/FXSAVE and NX capability. It allocates 64 KiB, writes and checks two indexed bit patterns through volatile accesses, then frees the allocation. This tests the allocated memory only. Browser builds display separate HTTP and pointer/wheel driver installation results. The existing ELF format, segment bounds, TLS and executable-entry checks now report their outcome. Payload allocation, stack and TLS preparation are shown before entering the runtime.

Successful startup displays the complete result for 750 ms, then starts the runtime. Failure displays a red item name and EFI status and waits for a key before returning to firmware. The CPL3 line describes the required startup policy; it does not claim that the sandbox was tested before its runtime checks execute. Renderer boundary tests remain separate.

## Verification

```sh
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
ENABLE_BLINK=1 ENABLE_MEMORY_PROBE=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/memory-probe" ./build-musl.sh
python3 tests/post-qemu.py
python3 tests/state-suite.py --workers 1 --build-directory build/blink \
  --require-scroll-wire --https-ca build/network-https/ca.esl \
  --prefix post-release renderer-scroll heavy-site
python3 tests/state-suite.py --workers 1 --build-directory build/memory-probe \
  --require-cpl3-probe --require-memory-probe --require-scroll-wire \
  --https-ca build/network-https/ca.esl --prefix post-isolation sandbox
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

The fixture server from `tests/network-servers.py` must remain running during the browser suites. `tests/post-qemu.py` runs QEMU/OVMF twice, once with the real image and once with only the embedded ELF magic damaged in a temporary copy. It checks actual loader messages and captures GOP output. It pauses QEMU after the target message to capture the screen; this is not evidence of subsequent browser startup. The separate `post-release` suite covers startup and interactive browser behavior.

Both POST cases passed. The successful screen shows each completed check. The corrupted image displays `EFI_UNSUPPORTED` (`0x8000000000000003`) and never enters musl or reports POST completion. Both screenshots were visually inspected; the failure item and retained diagnostic screen are legible.

The final normal-image suite (`post-release-suite.json`; local artifact) passed both root scrolling during JavaScript and real-site interaction after POST. The final probe-image suite (`post-isolation-suite.json`; local artifact) passed all 11 startup CPL3 boundary cases, 160 MiB allocation/reclamation at CPL0/CPL3, live forbidden-memory accesses and interactive recovery. Binary validation (`post-binary.log`; local artifact) also passed.

- Machine-readable POST results (`post-qemu.json`; local artifact)
- Successful POST screen (`post-success.png`; local artifact)
- Invalid ELF diagnostic screen (`post-invalid-elf.png`; local artifact)

Normal image SHA-256: `7f41014e7fe519ccaa4c9e95db1165560b1bb236c67f496e1d4166c5cfa60434`.

Memory/boundary-probe image SHA-256: `dc6b35c7ce6d3a499a21fe5e7c107a8ab3084cadb89947e8cda9e36c4f494022`.

CPU-feature rejection, injected memory-pattern errors and driver installation failures have not been fault-injected in QEMU. Physical hardware remains unverified.
