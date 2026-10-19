# UEFI BROWSER

An experimental x86-64 UEFI browser built from pinned Chromium components: Blink, V8 and Skia, with musl and Chromium's libc++ ABI. It runs through UEFI firmware services and GOP software rendering without a Linux guest.

The project is an independent port. It is not an official Chromium or Google product.

![UEFI BROWSER displaying a website in QEMU](validation/tabs-site.png)

## Project status

| Area | Current implementation |
| --- | --- |
| Web engine | Blink DOM, CSS and layout; V8 JavaScript; Skia CPU rasterization |
| Browser interface | Up to four tabs, address bar, Back, Forward, Reload, per-tab session history, settings, About and process monitor |
| Networking | Firmware HTTP/HTTPS with a Mozilla-derived CA bundle; restricted subresource support |
| Input | Mouse movement, buttons, vertical wheel and firmware keyboard input |
| Multiprocessing | On supported multi-CPU firmware, one AP renderer runs Blink/V8, layout and normal viewport rasterization with private image data, TLS and page tables; the BSP owns scroll-tile rasterization, native UI and firmware networking |
| Validation | QEMU/OVMF execution, real-site interaction, binary checks and clean firmware return |

The renderer address space still executes at CPL0 and retains firmware identity mappings. It is not a security sandbox. Per-tab processes, fault recovery, forced termination, general worker threads and broad website compatibility are not implemented. Physical hardware has not been validated. Native UI can run during a renderer task; DOM input dispatch and page scrolling still wait for synchronous JavaScript or layout to finish.

Validation evidence: [AP renderer and service separation](validation/RENDERER-PROCESS.md), [tabs and process monitor](validation/TABS-AND-MONITOR.md), [idle-resume results](validation/IDLE-RESUME.md), [process isolation](validation/PROCESSES.md), [settings and About](validation/FLUENT-SETTINGS.md), and [input under load](validation/HEAVY-INPUT.md). Website captures document individual test runs, not universal compatibility.

## AI-assisted development and disclaimer

This project was developed with substantial assistance from OpenAI Codex using an iterative workflow sometimes called "vibe coding." AI tools contributed to implementation, debugging, testing and documentation. This disclosure does not describe the authorship of upstream dependencies or imply that every change has received independent human review.

The browser is experimental. Recorded QEMU tests cover specific scenarios; they do not establish production readiness, a complete security sandbox or physical-hardware safety. Use a disposable test environment and avoid sensitive accounts or data. Human review and validation are required before accepting AI-contributed changes.

See [NOTICE.md](NOTICE.md) for the development disclosure, verification limits and operational disclaimer. The warranty disclaimer in [LICENSE](LICENSE) and the separate third-party licenses remain applicable; the notice adds no license restrictions.

## Quick start

From the repository directory on a Linux development host:

```sh
JOBS=16 ./build-blink.sh
./run-qemu.sh
```

The build downloads pinned upstream sources. See [build instructions](#build-the-browser) for tool versions and workspace layout. QEMU uses four virtual CPUs and 4 GiB of guest RAM by default. Source repositories do not need to contain generated EFI images or Chromium build archives.

## Navigation

**The browser starts with a blank page and an address bar.** Click the address bar or press Ctrl+L, type an HTTP(S) URL and press Enter. Addresses without a scheme use HTTPS. No website is loaded automatically by the default image. The runner opens an SDL window named `UEFI BROWSER`.

QEMU device-input tests verify address entry, URL replacement, page buttons, links, text editing and vertical wheel scrolling. The earlier `zeroday0619.dev` captures are website compatibility evidence rather than the default home screen. See [validation results](validation/RESULTS.md). Complete Chromium compatibility and every site interaction remain unverified.

## Tabs and process monitor

Use the `+` button or Ctrl+T to open a blank tab. Click a tab or use Ctrl+Tab / Ctrl+Shift+Tab to switch. Ctrl+1 through Ctrl+4 select a position; Ctrl+9 selects the last tab. Close a tab with its `x` button or Ctrl+W. Closing the last tab creates a new blank tab.

Each tab owns a Blink frame, page pixels, address edit state and session history. Switching preserves the document, form values, JavaScript state and scroll position. Background tabs are marked hidden and stop scheduling paint/raster work. They are not suspended processes: background JavaScript and networking can still run. The browser supports at most four tabs and refuses new tabs when the tracked C heap exceeds 96 MiB. These limits do not guarantee that arbitrary heavy pages will fit in memory. Tabs are not persisted across browser sessions.

Click **Processes** in the tab strip or press Shift+Esc to open the process monitor. Escape or the same shortcut closes it. The native monitor distinguishes the BSP UI/network service from the AP renderer and displays the browser heap and per-tab URL, navigation and presentation state. Historical transfer-worker counters remain available for the local-renderer path. Its refresh target is 500 ms; firmware service calls can still delay updates.

Tracked C heap size excludes virtual-memory commitments and AP page allocations. CPU percentages and per-tab memory are not measured. Tabs share one AP renderer when available. A single-CPU or unsupported-firmware configuration uses the local renderer. The monitor does not forcibly terminate the renderer; closing a tab queues its destruction until the current task returns. Opening the monitor hides the selected page and pauses its painting until the monitor closes.

## Browser settings

Click the gear at the right of the address bar or press Ctrl+, to open `about:settings`. The Fluent 2-inspired interface uses neutral surfaces, blue selection and focus accents, rounded controls and a settings sidebar. It renders locally through Blink without downloading a UI framework. Noto remains the bundled font.

Settings include light/dark browser chrome, the website default font size (14, 16 or 20 px), JavaScript, images and startup behavior (blank page or settings). Search filters the categories. Site settings apply to the next navigation or reload; explicit website CSS can override the default font size. The browser theme does not recolor websites. Reset requires confirmation. Internal settings retain JavaScript so the controls remain available when website JavaScript is blocked.

`about:version` displays the build's Chromium version and revision, V8 version, rendering backend, platform and update limitations. The sidebar links to this page. Back, forward and reload work with these internal pages.

Preferences are stored in a versioned UEFI variable. The runner keeps `build/blink/UEFI_BROWSER_VARS.fd` across launches and locks the profile against simultaneous use. Set `QEMU_VARIABLES` to an alternate firmware profile path to start a separate profile. Reset changes browser preferences only; browsing history remains session-only. Firmware write failures are displayed as session-only settings.

External pages cannot use settings links to change preferences. The embedder requires an internal current document, a matching requestor origin and transient user activation. Network redirects cannot enter internal pages. Chromium profile sync, password management, extensions and automatic updates are not implemented.

## Verified firmware components

- V8 15.8.18: JavaScript, BigInt, Unicode regular expressions, Promise microtasks and garbage collection.
- Skia: CPU paths, curves, gradients, alpha, antialiasing and GOP presentation.
- musl/libc++: 46 C runtime checks, containers, streams, locale, TLS and mutex checks.
- Firmware HTTP/HTTPS: response metadata, bounded binary bodies, chunk decoding, Mozilla certificate validation and request header forwarding. Unsafe headers are rejected before transport. The live `https://zeroday0619.dev` check retrieved HTTP 200 and 72,341 bytes on 2026-10-17.
- Virtual memory: an 8 GiB reservation without equivalent physical allocation, sparse page commitment, read-only and inaccessible protection, discard, partial unmapping, overflow rejection and cleanup. A private PML4 copy preserves firmware mappings and the original CR3 is restored before firmware return. V8 and Skia also passed with this memory implementation.

Evidence is under `validation/`. `virtual-engines-qemu.log` and `virtual-engines-binary.log` describe the new virtual-memory image. The `build/v8-musl` image is an earlier independently verified runtime build.

## Blink integration

The current sources provide:

- A UEFI Chromium message pump and in-process main-thread scheduler.
- A real non-composited WebView, main frame and widget owner.
- Network-document navigation through `CommitNavigation` and a network body loader.
- A native `LoaderFactoryForFrame` hook and queued subresource delivery using EFI HTTP.
- Non-composited animation ticks, lifecycle updates, Skia painting and GOP copying.
- An EFI browser entry point and complete static-library target followed by strict ELF/EFI linkage.
- Embedded Noto Latin and CJK fallback fonts, plus Blink's existing downloadable-font decoding path.
- Generated GRIT resources embedded in the payload, including upstream HTML default CSS and image resources, with Brotli/gzip decoding.
- In-process widget and policy-container receivers that retain cursor, touch, selection, tooltip and document-policy notifications.
- Per-navigation policy containers with HTTP CSP parsing through Chromium's existing parser.

The complete Blink browser archive and EFI payload link successfully. Binary validation finds no required unresolved symbols, Linux syscall instructions or PE imports; optional weak hooks are listed in the log. QEMU initializes Blink, creates the main frame and loads the site's CSS, JavaScript, JPEG and WOFF2 resources over verified HTTPS. Actual JavaScript runs through V8, including event-listener registration and `Image.decode()`. Document completion, framebuffer capture, browser teardown and return to firmware passed.

The non-composited widget retains event-listener state without dereferencing a compositor. Image decode requests run through Skia on a queued task so completion cannot mutate the caller's pending-request list during dispatch. UEFI reports thread CPU accounting as unsupported while elapsed-time clocks remain available. The runtime supplies read-only page checks, bounded frame-pointer stack collection and a discardable image cache.

The subresource implementation currently accepts same-origin GET requests. It rejects cross-origin requests, redirects, credentials, Range, keepalive and synchronous loading. A pinned EDK II HTTP driver yields at DNS, TCP and TLS wait points. A bounded cooperative network queue keeps each active firmware call on a guarded stack and returns control to the BSP UI loop between waits. The AP submits jobs and retrieves completed responses through a service channel; document callbacks run in its renderer loop. HTTPS/TLS processing remains in the firmware network stack on the BSP. Requests overlap through a cooperative EEVDF queue on the BSP. Eligible requests are selected by weighted virtual deadlines, with a 500 microsecond service target and 4:1 main-document/subresource weights. Firmware calls still yield cooperatively; the target does not preempt long firmware or Blink work. Requests are not executed on firmware application processors. Frame presentation is demand-driven and paced at a minimum 16 ms interval; static pages no longer repaint continuously. Animation callbacks, lifecycle/paint recording and raster work use separate scheduled tasks. Raster tasks replay an immutable Chromium PaintRecord into a staging surface in bounded-height strips; only a complete frame reaches GOP. Input can run between strips, and a document commit discards the previous document snapshot. HTML parsing retains a 4 ms cooperative budget on UEFI. Individual JavaScript calls, lifecycle updates and individual drawing operations still run to completion. Worker-dependent parsing optimizations are disabled in the entry point; general worker-thread support remains incomplete. General browser services and complete response-policy handling remain incomplete. Mouse movement, left/right clicks and keyboard events reach the Blink widget. Visible tooltips, zoom and autoscroll remain incomplete.

## Build the browser

```sh
JOBS=16 ./build-blink.sh
```

This prepares the pinned source graph, builds the complete browser archive, links `build/blink/payload.elf`, embeds it in `build/blink/esp/EFI/BOOT/BOOTX64.EFI`, and checks the linked binary. Any failure stops image creation. It does not substitute the old validation image for a failed Blink build.

`build-blink-libraries.py` builds the library graph separately. `--resolve-inputs` resolves missing GN inputs at pinned revisions. Compiler diagnostics that name an empty pinned Git dependency trigger exact-revision restoration and a build retry. Other compiler and linker failures remain errors. Dawn's code generator uses the Go version pinned by Dawn's DEPS rather than the system Go installation.

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 16
python3 tests/browser-qemu.py
```

The browser QEMU check requires the real Blink EFI image. It waits for frame creation, document completion and GOP presentation, captures the framebuffer, requests browser shutdown and verifies return to firmware. The captured image requires visual inspection; lifecycle markers alone are not proof of a correctly rendered website. The recorded image in `validation/blink-browser-screen.png` has been inspected.

Host tools used during validation: Clang/LLD 23.1.3, Ninja 1.13.2, QEMU 11.1.2 and OVMF 2026.08. Host generators run on Linux; firmware C/C++ objects use musl headers and the UEFI runtime. Pinned revisions are in `revisions.json`. Source checkouts and large intermediate archives reside under `../../work/chromium-build`.

## JavaScript execution

The Blink firmware configuration includes V8 Sparkplug and TurboFan. The default `BLINK_JAVASCRIPT_MODE=optimized` uses `--single-threaded --sparkplug --turbofan`. Maglev and concurrent compilation remain disabled. The existing virtual-memory implementation supplies executable mappings and page-permission changes. Set `BLINK_JAVASCRIPT_MODE=sparkplug` or `BLINK_JAVASCRIPT_MODE=interpreter` when running `build-musl.sh` to produce comparison images from the same libraries.

TurboFan reduces repeated computation cost but does not preempt arbitrary JavaScript; optimization itself is synchronous. Page callbacks and microtask checkpoints retain JavaScript run-to-completion semantics. Firmware cursor capture continues while the renderer is busy, but DOM dispatch can still wait for a long synchronous script or lifecycle operation. The controlled QEMU workload produced matching checksums and reduced the median computation from 9 ms to below the fixture's 1 ms resolution. Input latency did not improve uniformly. See [renderer validation](validation/RENDERER-INPUT-IDENTITY.md) for measurements and limits.

## Run an existing image

```sh
./run-qemu.sh
```

The default uses four virtual CPUs (`QEMU_CPUS=1` selects the single-CPU fallback) and opens `build/blink/esp/EFI/BOOT/BOOTX64.EFI` in an SDL window named `UEFI BROWSER`. It tries KVM first and falls back to TCG if KVM cannot initialize. Set `QEMU_ACCEL=tcg` or `QEMU_ACCEL=kvm` to require one backend. The automated browser tests default to TCG; `QEMU_TEST_ACCEL=kvm` selects KVM explicitly. If this image is missing, the runner builds it with `build-blink.sh`. The default display requires QEMU SDL support. VNC is optional.

The default image requires 4 GiB of guest RAM. Press F12 to end the browser session, then press a key to return to firmware. Mouse clicks, Tab, Enter, Space, text editing, PageDown and Ctrl+Home reach the page. UEFI timer capture queues pointer transitions while Blink paints. Ordinary links navigate the active tab. User-activated GET requests for new windows, including target=_blank links, open a new tab within the tab limit. New-window requests open foreground tabs. Independent windows and opener/WindowProxy semantics are not implemented. The EFI image embeds a patched EDK II VirtioInputDxe driver and reconnects existing virtio relative-pointer devices. The driver polls every 5 ms and queues complete virtio reports instead of overwriting button edges. A full report queue applies backpressure to the device ring. Eligible root and nested vertical wheel input uses the BSP scene compositor. Other wheel events reach DOM listeners first; uncancelled events scroll the hit-tested Blink scroll chain, including nested elements. Composited scrolling uses 80 CSS pixels per wheel tick. Renderer-path wheel events request two lines; horizontal wheels, high-resolution touchpad gestures and kinetic scrolling are not implemented. The text path uses firmware UTF-16 keystrokes and synthesized releases; IME composition, physical key-up tracking and held-modifier mouse gestures such as Ctrl+click are not implemented. Keyboard shortcuts use the modifier state attached to each firmware keystroke.

Select another image with `BUILD_DIRECTORY`. The runner does not silently substitute an image when that directory lacks an EFI executable.

## Independent runtime and transport checks

```sh
./build-v8-musl.sh
BUILD_DIRECTORY="$PWD/build/v8-musl" python3 validate-binary.py
BUILD_DIRECTORY="$PWD/build/v8-musl" python3 test-qemu.py
NETWORK_URL=https://zeroday0619.dev ./build-network.sh
python3 tests/network-qemu.py
```

To reproduce the sparse-memory engine validation without rebuilding the engine archives:

```sh
ENABLE_V8=1 ENABLE_SKIA=1 BUILD_DIRECTORY="$PWD/build/virtual-engines" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/virtual-engines" python3 validate-binary.py
BUILD_DIRECTORY="$PWD/build/virtual-engines" \
  REQUIRED_MARKER='PASS: 8 GiB virtual reservation' python3 test-qemu.py
```

Physical-memory queries read descriptors using the stride returned by [UEFI GetMemoryMap](https://uefi.org/specs/UEFI/2.11/07_Services_Boot_Services.html#efi-boot-services-getmemorymap).

Firmware memory operations currently require four-level x86-64 paging and a free lower-half PML4 slot. Five-level paging is rejected. There is no general filesystem, socket, executable-process loader or worker-thread implementation. The isolated presentation workers execute a fixed program. Physical hardware and broad web compatibility remain unverified. Human review and validation are required before production acceptance.

## Virtio wheel driver

`build-input-driver.sh` builds EDK II `edk2-stable202608` at the revision in `revisions.json`. It requires Git, GCC, make, NASM, Python 3 and the system libuuid development files. The minimal DSC builds VirtioInputDxe and its required libraries; it does not replace the OVMF firmware image. `build-musl.sh` includes the resulting driver when `ENABLE_BLINK=1`.

The patch reports Z-axis support only when the device advertises REL_WHEEL, accumulates signed wheel movement with saturation and clears consumed movement. The EFI loader loads the embedded driver through LoadImage/StartImage and reconnects only existing virtio relative-pointer controllers. Network and disk controllers remain owned by firmware. The driver remains resident after browser exit so firmware pointer services remain valid. This integration is validated with OVMF 2026.08 and QEMU virtio-mouse-pci; arbitrary firmware and physical mice are not verified.

Source: [TianoCore EDK II](https://github.com/tianocore/edk2/tree/2970e5699ba6267f3384ffab20f96647578aebc8). License: [BSD-2-Clause-Patent](licenses/EDK-II.txt). Local changes: `patches/edk2-virtio-wheel.patch`, `config/virtio-input.dsc`, `src/input-driver.h` and the Blink wheel fallback in `src/blink/browser_frame.cc`.

## Address bar

The 48-pixel address toolbar and 32-pixel tab strip are rendered outside the Blink viewport. Pointer coordinates are translated into the page viewport; toolbar clicks and editing keys are not delivered to page controls. The bar follows the committed document URL after link navigation. Ctrl+L or clicking the bar selects the current address. Ctrl+A, Backspace, Delete, Left/Right and Home/End support editing. Escape returns keyboard input to the page. Invalid URLs and transport failures leave the current page visible and mark the field red. HTTP(S), `about:blank`, `about:settings` and `about:version` are supported; credentials in URLs are rejected. This is an address field, not a search box.

The default build uses `about:blank`. `BLINK_START_URL` remains an explicit build-time override for integration tests; it is not set by the runner. The default startup performs no website request. IME composition, clipboard integration and advanced text selection are not implemented.

## Navigation buttons and session history

Back, Forward and Reload appear immediately before the address field. Back and Forward are disabled when there is no corresponding entry. A button activates when the mouse is released inside the button that was pressed.

History records committed Blink navigations, including typed addresses, links and fragment changes. Back/Forward use Blink history items and back-forward load types; same-document traversal stays in the existing document. Reload requests the current document again without appending an entry. A new navigation after going back discards the forward branch. Failed transport requests do not advance the history position. The address bar follows the committed URL.

The history is kept in memory for the current browser session, capped at 256 entries. It is not a persistent browsing-history database or a history-list menu. Cross-document traversal fetches the document again; no back-forward cache is implemented. General JavaScript-driven history traversal and Navigation API interception are not fully integrated.

## Licensing and distribution

Original project code is licensed under [BSD-3-Clause](LICENSE). Copyright (c) 2026, zeroday0619. Existing file-specific licenses, upstream copyright notices and third-party terms remain in force. This grant covers original contributions and does not relicense third-party material.

See [third-party licensing notes](THIRD_PARTY_NOTICES.md) and the notices in [licenses/](licenses/). Chromium's top-level BSD license does not replace Blink file-specific licenses or the licenses of linked dependencies, fonts and certificate data. The current notice collection is not a complete license manifest for the linked EFI image.

Before distributing a binary release, complete the dependency and notice inventory for that exact build and satisfy applicable source and relinking obligations. Preserve upstream copyright and license notices in copied or modified files. The development archive is not a license-audited release package.

For source publication, see [GitHub publication preparation](PUBLISHING.md).

### User-Agent

HTTP document and subresource requests and JavaScript `navigator.userAgent` use the same browser identity:

```text
Mozilla/5.0 (UEFI; x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/158.0.8110.0 Safari/537.36 UEFIBrowser/0.3
```

The Chrome version comes from the verified Chromium checkout at build time. `UEFI; x86_64` identifies the firmware environment and architecture, and `UEFIBrowser/0.3` identifies this browser. AppleWebKit and Safari are browser compatibility tokens. The syntax follows the [MDN User-Agent reference](https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/User-Agent). Standalone network probes without Blink use `UEFIBrowser/0.3 (UEFI; x86_64)`. User-Agent Client Hints use the same Chromium build version. In secure contexts, `navigator.userAgentData` exposes Chromium and UEFIBrowser brands, `platform: "UEFI"`, and `mobile: false`. `getHighEntropyValues()` supports the metadata supplied to Blink, including x86 architecture, 64-bit bitness, and full brand versions. Firmware platform version and device model remain empty because no normalized values are available.

HTTPS navigation sends `Sec-CH-UA`, `Sec-CH-UA-Mobile`, and `Sec-CH-UA-Platform`. Subresources use Blink's Client Hints and Permissions Policy checks. The transport suppresses UA Client Hints on HTTP. Main-document high-entropy `Accept-CH` persistence and `Critical-CH` navigation retries are not implemented. See [Client Hints validation](validation/CLIENT-HINTS.md).

### Network scheduling and presentation SIMD

The cooperative network queue removes head-of-line waiting between suspended requests while preserving each request's guarded stack and errno. The queue admits at most 128 jobs. It accounts elapsed execution between yields and selects an eligible virtual deadline; newly admitted jobs start at the queue's weighted virtual time. This is a browser request scheduler, not a port of the Linux kernel scheduler. Blink retains its existing task scheduler.

Isolated AP presentation workers use SSE2 for both BGR copying and RGB channel conversion. Scalar prefixes and tails handle alignment and odd widths; streaming stores are fenced before completion is published. No AVX CPU or extended-state requirement is introduced. These transfer workers are disabled when the separate AP renderer is active; its completed frames are copied to GOP on the BSP.

See [scheduler and SIMD validation](validation/SCHEDULER-SIMD.md) for measured request completion, CPU-count comparisons, regression commands, and remaining input delays.

### Google compatibility identity

HTTP(S) pages under `google.com`, `google.co.kr`, `youtube.com`, `gmail.com`, `googleapis.com`, `googleusercontent.com`, and `gstatic.com` use a Chrome-on-Linux compatibility identity. Host matching uses canonical URL domain boundaries, so names such as `google.com.evil.test` do not match. Other sites retain UEFIBrowser and UEFI identifiers.

The main document request uses its destination's identity. Subresources use their document origin's identity. The JavaScript User-Agent and User-Agent Client Hints use the committed document URL, with matching Chrome/Chromium versions from the pinned source. The identity change does not add missing web APIs, DRM, authentication support, or renderer isolation.

The isolated QEMU identity test routes only its fixture DNS names to a local HTTPS server and uses a temporary trust store. It compares actual request headers with navigator.userAgent, navigator.userAgentData, and getHighEntropyValues. It does not contact Google's production servers or modify host DNS configuration. See [renderer and identity validation](validation/RENDERER-INPUT-IDENTITY.md).

### Renderer input boundary

Native input crosses a pointer-free, versioned message boundary defined in `src/process/renderer_protocol.h`. Messages carry document generations and sequence numbers. A navigation commit discards input queued for the previous document. In multi-CPU mode the native UI first sends input through an acquire/release command ring to the AP renderer. Within that address space, ordinary input is dispatched immediately and nested input waits in a bounded queue serviced through Blink's input task runner. This avoids the extra latency observed when all input was deferred to a separate task. UEFI's scheduler executes one Blink task per firmware pump iteration, so task batching cannot skip native input polls between scheduled phases.

Main-thread input scroll animation is disabled. Dispatched wheel and keyboard scrolling apply without waiting for scroll animation frames. In the controlled workload, wheel-event response decreased from a 109.7 ms median to 26.8 ms; this changes the input scrolling behavior and is not a general rendering throughput claim.

Frame updates return to the message pump between layout and paint recording. Recording revalidates the complete lifecycle because input or scripts may have changed the document in between. Individual layout operations and JavaScript calls still run to completion.

`src/blink/browser_broker.h` connects each frame's browser interface broker to a local Mojo permission service. Unsupported permission queries, requests and revocations return `denied`; embedded permission controls return unsupported. This prevents unresolved permission promises from aborting Blink during garbage collection. It does not implement device access or permission prompts.

Cookies are disabled because a cookie service and HTTP cookie handling are not implemented. `document.cookie` reads return an empty string, writes are ignored, and cookie/storage-access capability queries report unavailable. This prevents a synchronous cookie request from waiting indefinitely on the renderer's own thread.

The loader copies the payload's writable ELF segment and TLS template for the AP, remaps the data segment in a private page-table root, and invokes a fresh C/C++ runtime on a separate stack. Blink/V8, layout and normal viewport rasterization execute there. Scroll-tile rasterization, native tabs, toolbar, cursor and GOP presentation run on the BSP. Firmware allocations, time, entropy, preferences and HTTP/HTTPS jobs are brokered on the BSP. The local permission-denial broker runs inside each renderer frame.

Renderer allocations reuse private small-object slabs and 1 MiB physical-page batches to reduce service round trips. Acknowledged frame publications prevent concurrent reads of partially painted pixels. Input commands reserve capacity for browser controls, coalesce ordinary pointer motion, and preserve up to 8,192 deferred page-input samples per tab; overflow is counted explicitly. Navigation discards deferred input for the previous document. Closing waits for renderer acknowledgement before reusing the frame slot. Shutdown services outstanding runtime/network requests and allows two seconds for a graceful renderer return before requesting an NMI. The MP completion event must confirm that the AP returned before its stack, TLS and page tables are freed.

The BSP composes eligible vertical wheel scrolling from serialized Blink paint scenes while the renderer runs JavaScript. Scenes preserve transform, rounded-clip, opacity and blend-group ancestry, scroll element IDs, scroll-chain containment and Chromium sticky-position constraints. Adjacent chunks with identical properties share a picture. The AP reuses unchanged serialized font and image resources between scenes. The BSP rebuilds its own Skia pictures, fonts and images from immutable bytes before acknowledging publication. It creates missing 512-by-256 pixel tiles on demand and retains at most 32 MiB of tile pixels and 1,024 tile entries, so scrolling existing recorded content does not stop at the previous three-viewport cache boundary. Tiles are cropped to their picture bounds. A Skia spatial index skips offscreen drawing operations, and unchanged pictures retain their tiles across renderer acknowledgements. Each scene permits up to 4,096 picture groups and 32 MiB of serialized resource/picture data; larger scenes use renderer dispatch.

Native and CSS scrollbar parts carry cached normal, hover, pressed and disabled paint variants. The BSP selects these variants on pointer input without waiting for the renderer, including pointer departure into the toolbar and native arrow disabling at scroll boundaries. Disabled CSS scrollbars reject input. A detached Blink scrollbar resolves custom pseudo-element styles without mutating the live control. Identical variants share serialized and decoded data, and tile keys include the selected appearance. Custom track pieces use full-track paint records clipped at the moving thumb midpoint. State-dependent geometry changes require a new renderer scene; gradients and borders tied to a changing track-piece size may differ during asynchronous motion. See [scrollbar appearance validation](validation/SCROLL-STYLE.md) for the 25-second JavaScript blocking test and remaining limits.

Absolute scroll commands identify the target scroll container and carry sequence acknowledgements. Nested regions scroll independently, boundary input can propagate through scroll ancestry, and `overscroll-behavior: contain` prevents propagation. Sticky constraints update visually before Blink processes the queued offsets. Navigation and recovery discard scene resources. Passive wheel listeners receive non-cancelable notifications after asynchronous scrolling. Blink blocking-wheel hit-test regions retain cancelable renderer dispatch; unrelated regions remain eligible. Arrow, PageUp/Down, Home/End and Space scrolling use the acknowledged focused scroll container when its keyboard event path has no listeners or editing/activation defaults. Native and CSS `::-webkit-scrollbar` controls are painted separately for both axes. Thumb dragging, arrow buttons, track paging and held-button repeat update the BSP scene while the renderer is busy. ArrowLeft/Right scroll eligible focused containers horizontally. Absolute commands carry both coordinates, including negative RTL offsets. Non-translation transforms, path clips, filters, scroll snapping and repaint-dependent backgrounds still use renderer dispatch. Cancelable wheel/keyboard handlers, text editing, JavaScript-built scrollbar widgets and scroll-linked JavaScript still wait for the renderer. Lazy or newly changed content requires a new renderer scene. Individual picture raster operations run to completion on the BSP; this is not a worst-case latency guarantee or Chromium's complete compositor. See [nested scroll and tile validation](validation/SCROLL-TREE.md) [asynchronous input validation](validation/SCROLL-INPUT.md) and [two-axis scrollbar validation](validation/SCROLL-CONTROLS.md).

The entropy broker uses a bounded shared bounce buffer before copying into renderer memory. Firmware must not write directly to an AP-only virtual mapping. Both the private-mapping runtime probe and a 65,536-byte JavaScript `crypto.getRandomValues()` regression cover this boundary.

Open **Processes** or press **Shift+Esc**, then click **End renderer** or press **Delete** to interrupt the AP, including infinite JavaScript. The loader uses an NMI and a private exception table to return to its supervisor. Recoverable divide, invalid-opcode, general-protection and page-fault exceptions use the same cleanup path. Runtime aborts in the AP raise an invalid-opcode exception. The BSP waits for AP quiescence and MP completion, abandons outstanding renderer network jobs, releases retained responses and broker allocations, and starts a fresh renderer. Active firmware requests keep their stacks until completion. Native tabs and address text remain, but all renderer documents, JavaScript state and per-tab session histories are discarded. Reload a tab or enter a URL to restore a document. Failed restart attempts leave the native UI available for another attempt.

The renderer still runs at CPL0 with firmware mappings and trusted broker pointers. This is not Chromium's security sandbox. Recovery requires an intact supervisor, interrupt delivery and a usable AP exception stack; arbitrary memory corruption, stack destruction, double faults and machine checks are not contained. Single-CPU fallback cannot independently interrupt its renderer. See [scroll and recovery validation](validation/SCROLL-RECOVERY.md) and the earlier [AP renderer validation](validation/RENDERER-PROCESS.md).
