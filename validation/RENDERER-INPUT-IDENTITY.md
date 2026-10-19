# Renderer input, JavaScript optimization and browser identity

## Implementation

- `config/blink-firmware.gn` builds V8 TurboFan. `build-musl.sh` selects optimized, Sparkplug-only, or interpreter execution without rebuilding the libraries. All modes remain single-threaded.
- `src/blink/frame_loop.cc` separates animation, layout, paint recording, and raster tasks. Recording revalidates lifecycle state after intervening input; navigation invalidates the old frame snapshot.
- `browser_frame.cc` disables main-thread input scroll animation so wheel and keyboard scrolling do not wait for animated intermediate positions.
- `patches/blink-build.patch` selects a one-task batch in the UEFI scheduler. The entry point also disables the time-based batching feature. Native input is polled between individual tasks instead of after a batch.
- `src/process/renderer_protocol.h` defines a 56-byte pointer-free input message with protocol version, document generation, sequence number, and pointer/key payload. `browser_frame.cc` consumes ordinary input immediately and queues nested delivery. Navigation discards stale queued input. Queue and dispatch delays are measured separately.
- `src/blink/browser_broker.h` supplies each frame with a Mojo browser interface broker and a permission service. All currently unsupported capabilities return denied; embedded permission requests return unsupported. Observers remain connected until their clients disconnect or the frame closes.
- `src/blink/browser_identity.h` centralizes default UEFI identity and Google-scoped Chrome identity. Main-document headers use the destination URL, subresource headers use the document origin, and JavaScript uses the committed document URL.

These changes do not move Blink, V8, layout, or Skia rasterization into an AP process. Existing isolated AP workers only present completed pixel buffers. Process-local runtime/TLS/allocation, executable loading, firmware and network brokering, and renderer recovery remain required. The input and Mojo service boundaries currently execute in the main process.

## Controlled input measurements

The workload creates 800 layout cards and repeatedly executes a 500,000-iteration integer calculation. QEMU/KVM used four vCPUs and 4 GiB RAM. Each run sent five keyboard, click, and wheel sequences through emulated input devices while work continued. The fixture checks every logged calculation against an independent affine-exponentiation implementation.

| Configuration | Computation median | Keyboard median / maximum | Click median / maximum | Wheel median / maximum | Maximum logged poll gap |
| --- | ---: | ---: | ---: | ---: | ---: |
| Sparkplug, existing batching | 9 ms | 5.78 / 57.30 ms | 51.94 / 57.16 ms | 88.60 / 135.11 ms | 101.771 ms |
| TurboFan, existing batching | 0 ms | 10.74 / 134.83 ms | 46.76 / 46.89 ms | 88.22 / 114.39 ms | 115.625 ms |
| TurboFan, one-task batches | 0 ms | 16.19 / 62.26 ms | 36.37 / 46.70 ms | 78.28 / 119.54 ms | 80.000 ms |
| Above plus permission/cookie fixes | 0 ms | 10.83 / 56.96 ms | 46.80 / 51.81 ms | 109.72 / 150.40 ms | 78.050 ms |
| Final image: immediate input scrolling | 0 ms | 10.93 / 52.48 ms | 46.73 / 103.37 ms | 26.81 / 37.08 ms | 72.034 ms |

Computation durations are rounded to integer milliseconds; zero is a rounded measurement, not zero execution time. Input measurements run from host QMP submission to the guest DOM log, with 5 ms polling. Click measurements include a 30 ms button hold. They are not optical display latency or statistically robust tail percentiles. The comparison supports a large reduction in repeated JavaScript computation and shows mixed input results. It does not establish a general website speedup. TurboFan compilation remains synchronous and can itself delay input. The renderer queue timer starts after native polling, so its microsecond-scale result excludes time spent waiting for that poll.

The one-task run measured a 23.853 ms maximum explicit layout phase and a 41.758 ms maximum input dispatch. These intervals can still block the main thread. Reentrant input dispatch is prevented; arbitrary scripts and individual lifecycle operations are not interrupted.

An all-asynchronous input adapter was also tested and rejected: its keyboard median increased to approximately 16 ms from a roughly 6 ms baseline. The retained adapter avoids that extra task hop. Raw measurements are in `renderer-input-results.json` and `renderer-{sparkplug,turbofan,unbatched}-heavy.log`. These comparisons precede the permission-broker addition; subsequent final-image runs are recorded separately.

The final two rows isolate the input scroll-animation setting from the other source changes. Wheel response improved, while keyboard medians were similar and the click maximum regressed in this small sample. No blanket input-latency improvement is claimed. The final image recorded a 27.059 ms maximum explicit layout phase and a 65.442 ms maximum input dispatch. Additional logs are `renderer-broker-heavy.log` and `renderer-immediate-scroll-heavy.log`.

## Website and service validation

The real HTTPS site `zeroday0619.dev` was loaded through the native address bar. Input began while resources were still loading, and Blink logged changing scroll offsets. After load, a real click and a Space key changed the site's theme visibly; wheel scrolling, tab switching, restored-tab interaction, and closing the website tab while retaining settings also passed. Screenshots were inspected for rendered content and theme changes.

In the initial one-task run, the visible theme response was 211.7 ms for a click and 60.6 ms for Space. Click timing includes a 30 ms button hold and screenshot polling. The longest logged input poll gap during the live-site run was 192.938 ms. Loading-time input was exercised, but this does not establish a maximum click/key response during every loading phase. Remaining long JavaScript and lifecycle tasks are not solved by the new boundaries.

The final immediate-scroll image repeated the live-site test: visible theme response was 86.3 ms for click and 85.4 ms for Space, with a 194.407 ms maximum logged input poll gap. The input batch began during loading and logged scroll offsets 40, 80, 710 and 713. The screenshot captures native address-field typing during that batch. Page-button and keyboard-triggered theme changes are asserted after load, not during every loading stage. Final screenshots are `renderer-heavy-{loaded,click,keyboard,loading-0,scroll}.png`; logs and loading samples are `renderer-site.log` and `renderer-site-samples.json`.

Google's production homepage exposed a separate crash: an unanswered `navigator.permissions.query()` resolver was garbage-collected. Symbolization identified `blink::Permissions::query` as the resolver creation site. Suppressing the resolver DCHECK would leave the request unresolved. The local permission service instead supplies terminal responses. The HTTPS identity fixture additionally checks five permission queries, denied states, and rejection of an invalid permission descriptor.

After that fix, GDB identified an input freeze in `Document::cookie` → `RestrictedCookieManagerProxy::GetCookiesString` → `mojo::SyncHandleRegistry::Wait` → `pthread_cond_timedwait`. The browser has no cookie service, and the renderer cannot process its own pending broker request while synchronously waiting. The runtime repeatedly returned ENOSYS for futex waits. This was a service deadlock, not a long-running JavaScript calculation. Cookie support is now explicitly disabled in frame settings, and storage-access capability checks honor that setting before reaching synchronous cookie IPC. The fixture checks empty cookie reads, ignored writes, `navigator.cookieEnabled === false`, and `document.hasStorageAccess() === false`. See `renderer-google-hang-stack.log` for the captured guest stack. Arbitrary unimplemented browser services remain outside this fix.

The repaired image accepted the real Google search text `uefi specification`, displayed live autocomplete suggestions, and submitted the search through Enter. Google subsequently returned HTTP 429 and a traffic-verification page. Search results are not verified. The query screenshot was inspected (`renderer-google-query.png`); `renderer-google.log` preserves the navigation and clean firmware return with incidental Google query tokens redacted. This live Google run preceded the final input-scroll setting change. No traffic-verification bypass was attempted.

The HTTPS service/identity fixture passed all three origins, five permission queries per origin, invalid-descriptor rejection, cookie read/write checks, storage-access checks, and EFI shutdown. Evidence is in `renderer-services.log`. These tests establish the implemented identity and service behavior, not complete Google product compatibility.

The final image also passed `INPUT_SCENARIO=tabs`: DOM/form-state preservation, independent tab histories, pending resource-load survival after closing another tab, target-blank navigation, tab limits, process monitoring, and shutdown. See `renderer-tabs.log`. `validate-binary.py` passed the static EFI/load bounds, required-symbol, and syscall-instruction checks; `tests/pump-loop.sh`, Python compilation, shell syntax checks, and `git diff --check` passed. The final image hash is in `renderer-image-sha256.txt`, with binary results in `renderer-binary.log`.

## Google compatibility identity

Chrome identity is enabled for these roots and their subdomains only: `google.com`, `google.co.kr`, `youtube.com`, `gmail.com`, `googleapis.com`, `googleusercontent.com`, and `gstatic.com`. Canonical URL-domain matching excludes names such as `google.com.evil.test`. This is an explicit allowlist, not an exhaustive list of every Google-owned domain.

The pinned Chromium source identifies itself as version 158.0.8110.0. Allowed destinations use a Linux x86-64 Chrome User-Agent, Chromium/Google Chrome brands, and Linux platform metadata. Other destinations retain UEFIBrowser/UEFI. This keeps the header and JavaScript identity surfaces consistent with the [Chromium User-Agent guidance](https://chromium.googlesource.com/chromium/src/+/main/docs/user_agent/) and [Client Hints documentation](https://developer.chrome.com/docs/privacy-security/user-agent-client-hints). It does not add Chrome services, authentication support, DRM, or general browser compatibility.

`tests/google-identity-qemu.sh` starts a loopback-only HTTPS and DNS fixture with a temporary CA. Its DNS redirection applies only to the test process; host DNS configuration and the normal Mozilla trust bundle are unchanged. Actual HTTPS headers are checked against `navigator.userAgent`, `navigator.userAgentData`, and high-entropy JavaScript metadata for Google, a lookalike domain, and an ordinary IP destination. Document and script requests must each contain exactly one matching User-Agent and each baseline Client Hint. This fixture does not establish high-entropy HTTP Client Hints or Accept-CH persistence.

## Reproduction

Run from the project directory. QEMU tests share the build ESP and must run sequentially.

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
./tests/pump-loop.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
./tests/google-identity-qemu.sh
```

Start `python3 tests/redirect-server.py` in another terminal for the workload and local tab fixtures, then run:

```sh
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=heavy-fixture python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=tabs-site python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=google python3 tests/browser-qemu.py
```

For the Sparkplug comparison, relink with `BLINK_JAVASCRIPT_MODE=sparkplug`; use `optimized` to restore TurboFan. These are QEMU results. Physical firmware behavior and complete renderer process separation remain unverified.
