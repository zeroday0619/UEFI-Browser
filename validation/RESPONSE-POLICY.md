# Cross-origin resource policies

The previous loader rejected cross-origin `no-cors` subresources with `ERR_NOT_IMPLEMENTED`. This prevented ordinary external images, stylesheets and classic scripts from loading. The loader now calls the original Chromium ORB analyzer and CORP implementation from pinned revision `f12c350cb1e1e7ea05c11d6045b0a04158ec9e28` before delivering responses to Blink consumers. This is a web-platform integration, not completion of the sandbox or Chromium port.

## Implementation

`src/blink/resource_loader.cc` checks CORP on every response, including redirects, and ORB on the final response. The analyzer examines headers and at most the first 1,024 body bytes, then uses the original end-of-sniffing decision. Blocked subresources fail with the original network error. The pinned implementation's default behavior for blocked script-initiated opaque fetches returns stripped headers and an empty body. Blink's opaque response filtering and canvas/CSS origin checks remain active.

The initiating origin comes from the document or worker context. Redirects retain the tainted request origin, so returning to the initial origin does not bypass ORB. Each document owns an ORB media-response history through a document supplement; workers use separate sequence-local histories. Loaders retain that state during outstanding requests. Histories are cleared after 256 entries, conservatively requiring later requests to pass fresh checks. Navigation replaces document state instead of retaining policy through a reused window.

`src/blink/navigation.cc` parses COEP with Chromium's parser. Subresource loading enforces `require-corp`; `credentialless` suppresses eligible cross-origin no-cors cookies and checks the response using the resulting credentials state. Workers inherit the creating context's policy. Both policy analysis and Blink MIME metadata use Chromium's duplicate Content-Type extraction. Invalid header names or values fail rather than reaching header-builder assertions.

The frame-loader hook in `patches/blink-build.patch` supplies the owning window so the adapter can obtain the document's origin and policy. `src/blink/response_headers.h` converts the bounded firmware headers without flattening duplicate fields. No new external dependency was added.

## Verified behavior

`orb-suite.json` records the initial 29-case policy fixture and successful HTTPS network, worker, MSE Opus output and DevTools regressions. `orb-document-suite.json` adds worker COEP inheritance and repeated iframe navigation, reaching 33 policy assertions with the same regression suites passing. `orb-final-baseline-suite.json` validates 35 assertions after aligning duplicate MIME extraction; its separate delayed-response case reproduces the firmware timeout described below.

The policy fixture covers executable cross-origin scripts, applied stylesheets, PNG decoding, inaccessible cross-origin CSS rules, tainted canvas readback, CORP allow/deny cases, HTML/JSON/XML sniffing, nosniff and never-sniffed MIME types, rejection of an unverified middle-of-resource range response, CORS approval, opaque fetch filtering, redirects, COEP cookies, worker requests and document policy replacement. Successful runs require renderer CPL3 and no unexpected renderer faults. `tests/input-pages/response-policy.html` and `tests/response-policy-fixture.py` contain the assertions and deterministic responses.

## Firmware timeout reproduction

With EFI `55870b0a25d34974b02c3d38dd93124ca71ceafb064f8ab2c4f048f8c6cec212`, a local HTTPS endpoint delaying its headers for six seconds failed with EFI status `0x8000000000000012` despite the browser configuring a ten-second blocking timeout. The baseline serial log is `orb-final-baseline-transport-idle.log`. Both header and TLS-body waits in EDK II `HttpImpl.c` used the five-second `PcdHttpIoTimeout` instead of the instance's configured timeout.

The HTTP patch now uses `TimeOutMillisec` for both waits, retaining the PCD fallback when the configured value is zero. Timer arithmetic widens before multiplication. The regression fixture includes six-second header/body waits, expiration of a response delayed beyond the ten-second setting, and a subsequent successful request. This does not disable timeout enforcement or change TLS verification.

`orb-timeout-transport-idle.log` records successful header and body waits of 6,040 ms and 6,025 ms, followed by rejection at 10,026 ms and a successful recovery request. The same image passes the 35 response-policy assertions. Its EFI SHA-256 is `94d711c0fe3942329da0313acd8512a46444cc1b60d5373c79111c64f021d978`; matching payload SHA-256 is `8b2c584423110d8f5807a7e92a7fb4115fc0b601d35fe06da3203dcde21c446d`. `orb-release-sources.json` records the source identities and verifies the native source mirror. Native compilation, EFI linking and `validate-binary.py` passed.

The final `orb-timeout-suite.json` contains seven passing scenarios: response policies, configured timeout, HTTPS network behavior, large fixed/chunked responses, workers, MSE Opus output and DevTools. `orb-frame-suite.json` additionally passes same-origin, cross-origin, sandboxed and CSP-inheriting frame checks. All eight executions report CPL3 and no unexpected renderer faults. The verified EFI and matching payload were promoted to `build/blink` after checking the profile lock and live QEMU processes. `orb-default-promotion.json` retains the previous image identities and backup paths.

Start the test origins with `python3 tests/network-servers.py --directory build/orb-timeout-https`, then run:

```sh
python3 tests/state-suite.py --cpus 4 --workers 2 \
  --build-directory build/orb-timeout \
  --https-ca build/orb-timeout-https/ca.esl --prefix orb-timeout \
  --require-cpl3 --require-no-renderer-faults \
  response-policy transport-idle network-secure large-resources workers mse-audio devtools
```

## Remaining boundaries

The checks currently execute inside the CPL3 renderer after the firmware response has been copied there. They restrict normal web-platform consumers but cannot protect response bytes from compromised renderer-native code. Enforcing origin locks and response policies in a trusted network service before copying into the renderer remains necessary for the complete sandbox goal.

Full navigation embedding policy, worker response-policy negotiation, COEP report-only reporting, Document Isolation Policy and cached opaque response policies remain incomplete. Media range-history reuse, general resource exhaustion and concurrency, and the full relevant WPT suites are not covered by these fixtures. The ORB implementation retains the behavior and documented specification deviations of the pinned Chromium revision. Successful local fixtures do not establish complete YouTube compatibility.
