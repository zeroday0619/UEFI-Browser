# Validation results

## Blink website rendering

On 2026-10-17, the real Blink/V8/Skia EFI image loaded `https://zeroday0619.dev` in QEMU with OVMF, 4 GiB guest RAM and software raster output to GOP. Visual inspection of `blink-browser-screen.png` confirmed the site's Korean headline, body text, navigation, buttons, three information cards and JPEG image at 1280×800. The image is a QEMU framebuffer capture, not a host-browser screenshot.

| Validation | Result | Evidence |
| --- | --- | --- |
| `python3 build-blink-libraries.py --resolve-inputs --jobs 20` | Passed | `blink-native-library-build.log`; complete native browser archive. The common time-header correction rebuilt 10,926 scheduled tasks without interruption. The final decode correction then passed an incremental rebuild. |
| `ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh` | Passed | `blink-efi-link.log`; actual ELF payload and `BOOTX64.EFI`. |
| `BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py` | Passed | `blink-binary.log`; bounded static x86-64 ELF loads, no required unresolved symbols, Linux syscall instructions or PE imports. Optional absent weak hooks are listed explicitly. |
| `python3 tests/browser-qemu.py` | Passed | `blink-browser-qemu.log`; main frame creation, document completion, CSS/JavaScript/image responses, GOP capture, engine cleanup and return to firmware. |
| Visual inspection | Passed for the initial viewport | `blink-browser-screen.png`; Korean text, styling, cards, icons and image are visible. Pixel-perfect equivalence and all interactions were not tested. |
| Runtime probes in the same boot | Passed | Sparse 8 GiB reservation, page protection, RNG/clocks, bounded frame-pointer backtrace, atomic load/Rust mmap64 ABI, discardable-cache retention/purge/accounting and 46 musl checks. |
| Exported Chromium patch consistency | Passed | `git apply --reverse --check patches/blink-build.patch` against the patched pinned tree. |
| Shell and Python syntax | Passed | `bash -n` for build/run scripts and `py_compile` for resource, link-input and QEMU scripts. |

The main page received HTTP 200 resources with these logged types and sizes: JPEG 34,031 bytes; WOFF2 2,057,688 bytes; CSS 26,395 bytes; JavaScript 1,845, 218,916, 137,413, 133,438, 11,082, 21,862, 890 and 77 bytes. These are fetched resources, not embedded substitutes for the site.

## Corrections validated by this run

- Native archive closure includes Chromium's actual C++ and Rust runtime archives. No required symbol was replaced with a success stub.
- GRIT default CSS and image resources are embedded and decoded with the pinned Brotli/zlib implementations.
- Frame and widget hosts use in-process Mojo receivers. Each navigation has a policy container and HTTP CSP conversion through Chromium's parser.
- Navigation and subresource bodies are delivered in queued 16 KiB chunks. Image resources receive the normal byte-stream API rather than the background-processor-only segmented-buffer variant.
- Non-composited event notifications retain listener state and host notifications while avoiding absent compositor and input-queue objects.
- `Image.decode()` performs a real Skia software decode and queues completion. The previous synchronous fallback modified the pending-request vector during iteration and failed its state check.
- Thread CPU accounting is advertised as unsupported. The previous optional scheduler sampling attempted `CLOCK_THREAD_CPUTIME_ID`, which firmware does not implement.
- Read-only memory validation reads actual page permissions. `mprotect` rounds nonzero lengths to the page boundary. The runtime reports its actual virtual reservation limit.
- The discardable image cache preserves locked data, supports retained unlocked data and purges only unlocked blocks. Stack diagnostics use bounded frame-pointer traversal; DWARF unwinding is not implemented.

## Execution and limits

`./run-qemu.sh` selects the Blink image and opens an SDL window named `UEFI BROWSER`. The GTK module dependency issue is avoided by the SDL display backend. `BUILD_DIRECTORY` still selects an explicit alternate image. Missing custom images fail instead of falling back to an unrelated validation program.

Mouse/keyboard page interaction and keyboard scrolling are now exercised as documented below. General browser services, worker threads and broad website compatibility remain unfinished. Vertical wheel input is verified in the wheel integration section below. The resource loader accepts same-origin GET requests and rejects unsupported cross-origin, redirect, credential-header, Range, keepalive and synchronous requests. Firmware transfers poll synchronously within queued work; this is not parallel networking. Complete COOP/COEP and other response policies remain incomplete. Physical hardware, pixel-perfect comparison, WPT and clean-room rebuilds were not tested.

Toolchain: Clang/LLD 23.1.3, Ninja 1.13.2, QEMU 11.1.2, OVMF 2026.08, V8 15.8.18 and musl 1.2.5. Chromium, Skia and the C++ runtime revisions are pinned in `../revisions.json`. Host generators execute on Linux; the guest image runs directly under UEFI without a Linux guest.

`blink-native-build-status.json` describes the library-only invocation and does not validate its subsequent EFI link or QEMU run. `blink-browser-result.json` records the completed integration result and artifact hashes.

## Earlier independent checks

The earlier V8/Skia runtime checks remain recorded in `v8-musl-uefi.log`, `v8-musl-binary.log` and `v8-musl-screen.png`. Sparse virtual-memory checks are in `virtual-engines-qemu.log` and `virtual-engines-binary.log`. Header forwarding and unsafe-header rejection are in `network-header-qemu.log`. The transport checks also verified rejection of invalid TLS certificates. These logs describe their individual images and do not replace the current Blink browser test.

Human review and validation are required before production acceptance.

## Blink input integration — 2026-10-18

The current image sends firmware pointer and keyboard events through WebFrameWidget::HandleInputEvent. A 5 ms UEFI callback captures pointer transitions into a bounded 256-event queue. Blink handles hit testing, DOM listeners, focus, editing and navigation. Tests use QMP device input; they do not inject JavaScript or invoke DOM controls directly.

- Real site: mouse dark-theme toggle, keyboard Space activation, English-language toggle (visually inspected), PageDown scrolling, Ctrl+Home and Projects fragment navigation passed.
- Local HTTP fixture: typing `abcq`, Backspace, Ctrl+A, Shift+A and punctuation produced `A-!`. Escape reached the DOM. Tab and Space activated the button. A mouse link loaded the second document. Tab and Enter loaded a target=_blank third document in the existing viewport.
- Both scenarios completed F12 shutdown and returned to the EFI loader without a fatal assertion or firmware exception.
- At this earlier checkpoint, wheel reports were unavailable from the installed OVMF driver (`wheel=0`). The subsequent wheel integration below supersedes that limitation.

Commands:

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
INPUT_SCENARIO=site-no-wheel python3 tests/browser-qemu.py
python3 -m http.server 8765 --bind 127.0.0.1 --directory tests/input-pages
# Run the following commands in another shell while the fixture server is active.
ENABLE_BLINK=1 BLINK_START_URL=http://10.0.2.2:8765/index.html BUILD_DIRECTORY="$PWD/build/blink-input-test" ./build-musl.sh
INPUT_SCENARIO=form BUILD_DIRECTORY="$PWD/build/blink-input-test" python3 tests/browser-qemu.py
```

Evidence: `blink-input-site-partial.log`, `blink-input-fixture-qemu.log`, `blink-input-binary.log`, `input-english.png` and `input-project-link.png`. Earlier logs and hashes describe earlier images. IME composition, physical key-up tracking, independent windows, POST navigation and arbitrary external navigation are not verified or fully supported. The input queue can drop its oldest sample if more than 256 reports accumulate without main-loop consumption.

## Virtio wheel integration — 2026-10-18

The approved EDK II driver is built at `2970e5699ba6267f3384ffab20f96647578aebc8` and embedded in the Blink EFI application. Boot logs confirm the replacement pointer driver and `wheel=1`. The first wheel run exposed an invalid gesture dispatch: WebFrameWidgetImpl requires compositor handling for scroll gestures. The adapter now dispatches the cancellable DOM wheel event and uses Blink's existing main-thread scroll chain for the default action. The assertion remains enabled.

QMP device-input checks passed:

- The live site scrolled through actual virtio wheel reports. Theme-button mouse clicks, Space activation and fragment-link navigation remained functional.
- A non-passive DOM listener calling preventDefault stopped wheel scrolling.
- The nested overflow element scrolled while the document remained stationary.
- Document wheel-down increased scrollY; wheel-up reduced it.
- Text editing and click/Tab/Enter document navigation passed again with the new driver.

Reproduction commands, run from the project directory:

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
INPUT_SCENARIO=site python3 tests/browser-qemu.py
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 -m http.server 8765 --bind 127.0.0.1 --directory tests/input-pages
# Run the following commands in another shell while the fixture server is active.
ENABLE_BLINK=1 BLINK_START_URL=http://10.0.2.2:8765/index.html BUILD_DIRECTORY="$PWD/build/blink-input-test" ./build-musl.sh
INPUT_SCENARIO=wheel-fixture BUILD_DIRECTORY="$PWD/build/blink-input-test" python3 tests/browser-qemu.py
INPUT_SCENARIO=form BUILD_DIRECTORY="$PWD/build/blink-input-test" python3 tests/browser-qemu.py
```

Logs: `input-driver-build.log`, `blink-wheel-link.log`, `blink-wheel-site.log`, `blink-wheel-fixture.log`, `blink-wheel-form.log` and `blink-wheel-binary.log`. Screenshots: `input-wheel-scroll.png`, `input-wheel-nested.png` and `input-wheel-document-down.png`. `blink-wheel-result.json` identifies the tested binaries by SHA-256. Earlier result files describe their earlier images.

The default action uses two line scrolls per wheel tick. Horizontal wheel input, precise touchpad deltas, kinetic scrolling, IME composition and physical-hardware validation remain outside the verified implementation. No independent windows or general cross-origin browser behavior is claimed.

## Address bar and blank startup — 2026-10-18

The default `build/blink` image now starts at `about:blank`. The address toolbar occupies the top 48 pixels, and the Blink viewport occupies the remaining height. Historical site screenshots and hashes above describe earlier images.

The QEMU address scenario verifies blank startup without resource downloads, typing an HTTP URL, Enter navigation, clicking a page button at its translated viewport position, Ctrl+L replacement, Tab/Enter link navigation and navigation back to `about:blank`. Screenshots show the address field changing to the committed document URL. The wheel fixture exercises scrolling after the viewport offset changed.

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
# Start the fixture HTTP server described above before running the address scenario.
INPUT_SCENARIO=address python3 tests/browser-qemu.py
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
ENABLE_BLINK=1 BLINK_START_URL=http://10.0.2.2:8765/index.html BUILD_DIRECTORY="$PWD/build/blink-input-test" ./build-musl.sh
INPUT_SCENARIO=wheel-fixture BUILD_DIRECTORY="$PWD/build/blink-input-test" python3 tests/browser-qemu.py
```

Evidence: `blink-address-qemu.log`, `blink-address-wheel.log`, `blink-address-binary.log`, `address-blank.png`, `address-loaded.png` and `address-link.png`. `blink-address-result.json` records the current default image hashes. The earlier site scenario requires an explicitly built site-start image; the default image no longer opens that site. Clipboard and IME input are not implemented.

## Navigation toolbar and session history — 2026-10-18

Back, Forward and Reload buttons were added beside the address field. The controller records actual Blink commits and stores the associated WebHistoryItem. Back/Forward use back-forward navigation commits; same-document traversal uses CommitSameDocumentNavigation. Reload uses the reload load type and replaces the current entry. Renderer-requested replacement loads preserve their load type.

QEMU tests used physical mouse reports through QMP, not direct calls to the controller:

- Initial Back and Forward buttons were disabled.
- A typed address and clicked link created distinct entries. Back and Forward loaded the expected documents through firmware HTTP.
- Reload ran the second document's script again while preserving the history count.
- Navigating to a new address after Back removed the former forward branch. Clicking disabled Forward did not navigate.
- Clicking a fragment link and then Back/Forward restored scroll offsets 0 and 1226. The final forward traversal did not reload the document.
- EFI static validation and shutdown/return to firmware passed.

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
python3 -m http.server 8765 --bind 127.0.0.1 --directory tests/input-pages
# Run the following command in another shell while the server is active.
INPUT_SCENARIO=history python3 tests/browser-qemu.py
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

Evidence: `blink-history-qemu.log`, `blink-history-binary.log`, `history-empty.png`, `history-reload.png` and `history-fragment-forward.png`. The current binary hashes are in `blink-history-result.json`. History is memory-only and limited to 256 entries. No persistent history database, back-forward cache or full JavaScript history-traversal implementation is claimed.

Changed files for this step: `src/blink/browser_entry.cc`, `src/blink/browser_frame.cc`, `src/blink/browser_frame.h`, `src/blink/navigation.cc`, `src/blink/navigation.h`, `tests/input-scenario.py`, `tests/input-pages/index.html` and `README.md`.

## Redirect navigation timing fix — 2026-10-18

The reported `document_loader.cc:1595` FetchStart DCHECK was reproduced with a real firmware HTTP navigation through 302 and 307 responses to a 200 document. The navigation adapter completed the network requests before creating the Blink document but left WebNavigationTimings empty. Blink's redirect handler therefore received a null FetchStart.

`src/blink/navigation.cc` now records navigation_start before navigation processing, fetch_start immediately before each firmware request and redirect_start/redirect_end around the redirect chain. The final fetch_start belongs to the final request. All address, link, reload and cross-document history navigation share this adapter. The upstream DCHECK remains enabled. No unobserved DNS, connection or response-header timings are synthesized.

Before-fix evidence: `blink-redirect-before.log` contains the same FetchStart DCHECK and runtime abort. After-fix evidence: `blink-redirect-after.log` reports redirectCount=2 and ordered relative timings of redirectStart=0.5 ms, redirectEnd=3311.5 ms and fetchStart=3311.7 ms in this run. These measurements are test observations, not performance guarantees. The final URL and rendered content are shown in `redirect-final.png`; reload and EFI return also passed.

```sh
python3 tests/redirect-server.py
# Run the following commands in another shell while the server is active.
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
INPUT_SCENARIO=redirect python3 tests/browser-qemu.py
INPUT_SCENARIO=history python3 tests/browser-qemu.py
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

The deterministic test server binds only to host loopback; QEMU reaches it at 10.0.2.2:8765. Changed files: `src/blink/navigation.cc`, `tests/redirect-server.py`, `tests/input-scenario.py` and this report. The user's original destination URL was not provided; the identical failure was reproduced and fixed with the local redirect chain.

History regression passed (`blink-redirect-history.log`), and static EFI validation passed (`blink-redirect-binary.log`). Current artifact hashes are recorded in `blink-redirect-result.json`.


## Google address-bar search and HTTP error bodies (2026-10-18)

The real EFI image was booted in QEMU from about:blank. QMP keyboard events entered https://google.com in the address bar. Google redirected to https://www.google.com/ and rendered its Korean homepage. Mouse and keyboard input entered `uefi specification` in the page's search field and submitted it with Enter. Blink committed the /search URL containing the entered query. The subsequent request redirected to /sorry/ and returned HTTP 429 with Google's unusual-traffic verification page. Search result content is **not verified**. No verification challenge was bypassed.

The original firmware adapter accepted EFI_HTTP_ERROR for response headers but rejected the same status while reading an HTTP error body. EDK II reports this status for body delivery on 4xx/5xx responses as well. `src/network/resource.c` now accepts it only when the already parsed HTTP status is at least 400. Existing body bounds, framing and completion checks remain active. `src/blink/navigation.cc` logs response status and body length so server rejection can be distinguished from transport failure.

Validation:

- `python3 build-blink-libraries.py --resolve-inputs --jobs 20`: passed.
- `ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh`: passed.
- `BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py`: passed; no required unresolved symbols or syscall instructions.
- Start `python3 tests/redirect-server.py`, then run `INPUT_SCENARIO=http-errors python3 tests/browser-qemu.py`: passed for 404, 429 and 500 bodies, document script execution and EFI return.
- `INPUT_SCENARIO=google python3 tests/browser-qemu.py`: actual address entry and search submission; Google verification response prevents confirming search results. Inspect `google-home.png`, `google-query.png`, `google-results.png` and `blink-google-qemu.log`.

Remaining compatibility limits include cross-origin subresource rejection and unsupported request modes. The Google run logs blocked gstatic and clients1 resources and a challenge-script ReferenceError. These are not reported as repaired. The default startup remains about:blank. The Google scenario recognizes HTTP 429 as a blocked external test outcome rather than treating a /search URL as proof of results.


## Pointer responsiveness (2026-10-18)

Two paths caused avoidable pointer stalls: each native input poll redrew the toolbar and copied the complete framebuffer, and cursor presentation waited for the main thread to return from firmware networking. EDK II can wait inside its HTTP protocol entry points, so servicing a cursor only between calls was insufficient. Queued movement could also replay obsolete positions after a stall.

`src/blink/browser_entry.cc` now presents pointer-only damage separately from full page frames. On validated RGB/BGR 32-bit GOP framebuffer modes, a pointer observer updates a saved-background software cursor directly from the existing input capture timer. The observer performs bounded framebuffer memory access without allocation, Blink calls or GOP calls. Full and toolbar blits exclude the timer with TPL_CALLBACK and restore/redraw the cursor around presentation. Unsupported framebuffer modes retain the GOP cursor fallback. `src/blink/input.cc` coalesces motion runs while keeping button edges and wheel events distinct; DOM dispatch remains on the Blink main thread. No additional dependency was added.

Controlled QEMU observations (TCG, 1280x800, blank page, 640 relative movements in four sweeps):

| Measurement | Before | After |
| --- | ---: | ---: |
| Full framebuffer presentations | 930 | 320 |
| Cumulative full-presentation time | 1,712,329 us | 631,563 us |
| Sweep endpoint reached | 4/4 | 4/4 |
| Cursor at new position during delayed HTTP response | No | Yes |

The movement test injects events every 5 ms and checks captured framebuffer pixels for the endpoint and cursor trails. Sweep durations remained approximately 0.89 seconds because injection pacing dominates this check; these are not optical input-latency or high-refresh frame-pacing measurements. The network test waits for the fixture server to receive a request, moves the pointer while the server holds the response for two seconds, and checks the new cursor position before document commit. The old EFI and new EFI were run separately for this comparison.

Commands and evidence:

- `python3 build-blink-libraries.py --resolve-inputs --jobs 20` and `ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh`: compiled and linked the updated EFI.
- `INPUT_SCENARIO=pointer-performance python3 tests/browser-qemu.py`: movement endpoint and no-trail checks; `pointer-before.log` and `pointer-pointer-performance-after.log` contain counters.
- Start `python3 tests/redirect-server.py` with its output redirected to a log. Set `NETWORK_SERVER_LOG` to that absolute log path, then run `EXPECT_RESPONSIVE=1 INPUT_SCENARIO=pointer-network python3 tests/browser-qemu.py`. `pointer-network-before.log` and `pointer-network-after.log` record the comparison; corresponding PNG files show the actual framebuffers.
- `INPUT_SCENARIO=form`, `INPUT_SCENARIO=wheel-fixture` and `INPUT_SCENARIO=history` with `python3 tests/browser-qemu.py` exercise page input and browser controls against the same local fixture server.
- `BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py`: see `pointer-binary.log`.

Remaining limits: document fetching still blocks Blink event handling, and full-page software rendering still runs on the single main thread. A responsive cursor does not imply that a click or script can execute during a blocked navigation. Firmware sections that mask timer callbacks can still delay cursor presentation. Physical hardware, high-refresh optical latency and unsupported GOP pixel formats were not tested in this run.


## Input during loading and demand-driven rendering (2026-10-18)

The previous image could move its software cursor during a firmware wait, but Blink still could not dispatch input. In the controlled delayed-response reproduction, the earlier image failed to deliver the typed `x` to the page before the test timed out (`responsive-loading-before.log`).

Changes:

- `patches/edk2-http-cooperative.patch`, `config/http.dsc`, `build-http-driver.sh` and `src/http-driver.h` build and load a pinned EDK II HTTP driver under a private service-binding GUID. DNS, TCP and TLS polling waits yield only at TPL_APPLICATION. The normal firmware HTTP service remains available to independent transport probes.
- `src/network/async.c` owns a serialized queue of at most 128 requests. Each active call uses a 512 KiB stack with inaccessible guard pages. The SysV switch preserves callee-saved registers, stack pointer, floating-point control state and per-job errno. The firmware ABI thunk retains its Microsoft x64 state on the suspended stack. No Blink code executes on these stacks. Abandoned active jobs retain their buffers and stack until firmware cleanup completes; abandoned queued jobs are skipped.
- `src/blink/navigation.cc` and `resource_loader.cc` submit work and deliver completion on the renderer task queue. A newer navigation invalidates the earlier commit callback. Address-bar logs distinguish an accepted navigation from its eventual commit, and transport errors are reflected after completion.
- The UEFI SegmentedBuffer response path now starts Blink's SharedBufferBytesConsumer. This preserves Fetch response-body lifecycle semantics rather than finishing a request with an unconnected placeholder body.
- `frame_loop.cc` paints only when Blink schedules a frame and limits presentation cadence to a minimum 16 ms interval. Static pages no longer run a permanent repaint loop.

### Runtime evidence

The fixture server holds a response until the test explicitly releases it. QMP events click a page field, type `x`, click a button and scroll the document while the response is still withheld. The test checks DOM logs and captures the painted framebuffer before releasing the response. Separate cases exercise a pending Fetch resource, a pending main-document navigation and replacement of a pending navigation with about:blank.

| QEMU event to DOM log | Pending Fetch | Pending main navigation |
| --- | ---: | ---: |
| Keyboard | 47.0 ms | 52.3 ms |
| Click | 46.4 ms | 57.0 ms |
| Scroll | 82.9 ms | 82.6 ms |

These are observed host-side injection-to-log times from one controlled run, including QMP overhead and button hold intervals. They are not physical display latency or guarantees for every website. Both cases passed the 500 ms per-event test bound before response release, then completed loading and returned to EFI. The replacement-navigation test confirmed that a stale response did not overwrite about:blank.

In the unchanged 640-movement blank-page test, the prior cursor-only optimization recorded 320 full presentations. The demand-driven candidate recorded 3 full presentations and 35,850 us cumulative full-presentation time, while retaining correct cursor endpoints and no trails. Candidate regression logs cover form input, links, wheel cancellation, nested scrolling, history, redirects, 404/429/500 response bodies and EFI return. Final-image checks and hashes are recorded in `responsive-result.json`.

Reproduction commands, run from the project directory:

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
export NETWORK_SERVER_LOG="$PWD/build/responsive-server.log"
export NETWORK_RELEASE_FILE="$PWD/build/responsive-release"
python3 tests/redirect-server.py > "$NETWORK_SERVER_LOG" 2>&1 &
server_pid=$!
INPUT_SCENARIO=responsive-loading python3 tests/browser-qemu.py
INPUT_SCENARIO=responsive-navigation python3 tests/browser-qemu.py
INPUT_SCENARIO=responsive-cancel python3 tests/browser-qemu.py
kill "$server_pid"
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

`tests/network-async.c` independently checks stack switching, errno isolation, request serialization and cleanup of an abandoned active request. `responsive-async-unit.log` records its host execution. Firmware runtime checks retain the existing strict binary validation and assertions.

Limits: network requests are cooperative and serialized, not parallel AP operations. Long JavaScript execution, an expensive indivisible layout/paint, and firmware work outside the patched wait points can still delay page input. Cross-origin resource policy, unsupported request methods and the previously observed Google traffic-verification response are separate compatibility limits. Physical hardware and high-refresh optical latency were not measured. Human review is still required for the new stack-switch and firmware-driver code.


### HTTPS wait verification

`responsive-https-final.log` verifies the same withheld-response Fetch test over TLS. Keyboard, click and scroll were observed at 41.5, 51.8 and 82.2 ms respectively before the server released its response. The request then completed and the image returned to firmware. Certificate verification remained enabled. A temporary fixture-only CA was supplied with `TEST_CA_FILE`; the production Mozilla store was not modified.

`tests/https-fixture.py --directory <temporary-directory>` generates the fixture certificate, a separate `ca.esl` and a loopback HTTPS server. After its ready message, run the browser test with `TEST_ORIGIN=https://10.0.2.2:8766`, `TEST_CA_FILE=<temporary-directory>/ca.esl`, `NETWORK_RELEASE_FILE=<temporary-directory>/release` and `NETWORK_SERVER_LOG` pointing to the server log. The fixture key and CA are test artifacts and are not included in the browser distribution.

`google-final.log` exercises the production trust store against Google's real HTTPS endpoint. Google navigation and search submission completed without a browser abort; search-result availability is still limited by Google's HTTP 429 traffic-verification response.


## JavaScript and rendering load follow-up (2026-10-18)

See [HEAVY-INPUT.md](HEAVY-INPUT.md) for the final Sparkplug, staged-raster and pointer-report queue implementation, actual-site comparison, regression commands, hashes and remaining latency limits.
