# User-Agent validation

The browser uses one identity function for HTTP requests and Blink Platform::UserAgent. Network-only probes retain a standalone identity. The HTTP transport ignores forwarded User-Agent fields to prevent duplicate or inconsistent identity headers.

Validation commands:

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
python3 tests/redirect-server.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=identity python3 tests/browser-qemu.py
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

The QEMU test enters the fixture URL through the address bar. The server echoes all received User-Agent values for the document and its external JavaScript resource. The page checks that each request contains exactly one User-Agent and both equal navigator.userAgent with UEFI and UEFIBrowser identifiers. The serial console reports USER_AGENT_TEST PASS. Firmware cleanup completed successfully. See user-agent-server.log and user-agent-qemu.log.

This verifies HTTP header and JavaScript identity consistency. HTTPS identity uses the same transport header construction; this test did not perform a separate HTTPS navigation. User-Agent Client Hints are outside this change.

EFI SHA-256: `b512d0f6d3e6b72b5aa8b8cbba4f1f17f00c0a0523b4967945313dce05d10b37`.
