# User-Agent Client Hints validation

## Implementation

`src/blink/browser_entry.cc` supplies Blink UserAgentMetadata and serializes the default brand header from the same metadata. Brands are Chromium (the verified checkout major version) and UEFIBrowser (major version 0, full version 0.3). The platform is UEFI. Architecture is x86 with 64-bit bitness; mobile and wow64 are false. Device model and platform version are empty rather than inferred from the build host.

`src/blink/navigation.cc` supplies the three default UA hints for HTTPS main-document requests, including each redirect destination. Subresources retain Blink's Client Hints generation and Permissions Policy checks. `src/network/resource.c` suppresses forwarded UA hints for non-HTTPS URLs. Standalone network probes have no browser Client Hints metadata.

The implementation follows the secure-context boundary described by [MDN Sec-CH-UA](https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Sec-CH-UA) and [navigator.userAgentData](https://developer.mozilla.org/en-US/docs/Web/API/Navigator/userAgentData).

## Reproduction

Run from the project directory, with the fixture servers in separate terminals:

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
python3 tests/https-fixture.py --directory ../../work/client-hints-tls
python3 tests/redirect-server.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 TEST_CA_FILE="$PWD/../../work/client-hints-tls/ca.esl" INPUT_SCENARIO=client-hints python3 tests/browser-qemu.py
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

The test uses an isolated temporary trust store. Production Mozilla roots are unchanged. QEMU enters the URLs through its keyboard input devices. The fixture echoes all received header values into the page, which compares the document and external script requests against the browser API. The policy fixture sends `Permissions-Policy: ch-ua=(), ch-ua-mobile=(), ch-ua-platform=()`.

## Scope

JavaScript high-entropy metadata is tested through getHighEntropyValues. Main-navigation high-entropy Accept-CH persistence and Critical-CH retries remain unimplemented. Cross-origin delegation, worker APIs, and HTTPS-to-HTTP redirects are not covered by this test. The transport's HTTP suppression is tested using direct navigation. This validation does not claim full Chromium Client Hints negotiation support.

## Results

Build, EFI link, static binary validation, and the four-CPU QEMU/KVM scenario passed. The HTTPS, HTTP suppression, and Permissions Policy assertions all passed. Firmware loader cleanup completed. Evidence is saved in client-hints-https.log, client-hints-http.log, client-hints-qemu.log, and client-hints-binary.log.

EFI SHA-256: `c3a2e853d841520c7305f852772d8ec0471f67ec497ff550f764a75659a1b6b1`.
