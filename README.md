# UEFI BROWSER

An experimental x86-64 UEFI browser built from pinned Chromium components: Blink, V8 and Skia, with musl and Chromium's libc++ ABI. It runs through UEFI firmware services without a Linux guest. The default renderer uses GOP software output; an opt-in Vulkan compositor presents through QEMU GPU buffers.

The project is an independent port. It is not an official Chromium or Google product.

![UEFI BROWSER displaying a website in QEMU](validation/tabs-site.png)

## Project status

| Area | Current implementation |
| --- | --- |
| Web engine | Blink DOM, CSS and layout; V8 JavaScript; Skia CPU rasterization |
| Browser interface | Up to four tabs, address bar, Back, Forward, Reload, per-tab session history, settings, About and process monitor |
| Networking | Firmware HTTP/HTTPS, IPv4 DNS and WebSocket/WSS with certificate verification; restricted subresource support |
| Web storage | Origin-keyed localStorage and Cache Storage snapshots on the profile disk, Blob body transfer and per-tab sessionStorage |
| Input | Mouse movement, buttons, vertical wheel, firmware keyboard input, libhangul two-set composition and Mozc Japanese conversion with candidates |
| Multiprocessing | On supported multi-CPU firmware, one AP renderer runs Blink/V8, layout and normal viewport rasterization with private image data, TLS and page tables; the BSP owns validated scroll-tile composition, native UI and firmware networking |
| Validation | QEMU/OVMF execution, real-site interaction, binary checks and clean firmware return |

Browser builds now run Blink, V8, workers and the software GPU at CPL3. The loader owns the page tables, GDT, IDT, TSS, exception stacks and fixed `int 0x80` syscall gate. Firmware mappings remain supervisor-only. An unavailable AP renderer fails startup instead of falling back to CPL0. Tabs still share one renderer and there is no per-site process isolation. V8 JIT pages use supervisor-managed write scopes: RX outside a scope and RW/NX inside one. The kernel applies the restriction to arena aliases and physical backing. See [JIT protection and validation](validation/JIT-WX.md). CPL3 asynchronous scrolling uses a bounded wire protocol: the renderer rasterizes picture tiles and the BSP copies, validates and caches compressed pixels and scroll metadata. No renderer C++ objects, font programs or Skia picture streams are decoded on this path. DOM event handlers still wait for the renderer. Native browser controls remain on the BSP. Physical hardware and independent security review remain unverified. See [CPL3 implementation and QEMU evidence](validation/RENDERER-CPL3.md) and [scroll transport validation](validation/SCROLL-WIRE.md).

The BSP service broker validates memory ownership and request bounds, copies firmware I/O arguments, restricts firmware variables and uses opaque network job IDs. In CPL3 mode the request slot remains supervisor-only, allocations use dedicated pages, and network responses are exported through read-only renderer mappings. See [CPL3 policy and verification](validation/RENDERER-CPL3.md) and the [earlier CPL0 broker validation](validation/RENDERER-BROKER.md).

The musl C heap grows on demand within a process budget derived from available firmware memory, capped at 1 GiB. Empty renderer slabs return their backing and the broker accounts for rounded physical pages. Privileged runtime aborts display recent diagnostics on the firmware console before halting; arbitrary supervisor exceptions are not yet connected. See [dynamic memory and fatal-display validation](validation/DYNAMIC-MEMORY.md) and the [full implementation goals](validation/IMPLEMENTATION-GOALS.md).

The loader displays a POST screen with CPU, a bounded writable-memory test, driver installation, ELF validation and payload setup results. Failures retain the item and EFI status before returning to firmware on a keypress. This is the browser's startup self-test, not a replacement for firmware hardware diagnostics. See [boot POST validation](validation/BOOT-POST.md).

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

## Runtime network and profile files

The runtime exposes IPv4 TCP sockets and a bounded file API to the browser and renderer through BSP-owned UEFI protocols. Descriptor ownership prevents accidental cross-owner access through the API. The default browser build reaches these services through the CPL3 syscall broker. WebSocket/WSS, localStorage/sessionStorage, Cache Storage and IndexedDB now connect Blink to these services. Cache Storage persists bounded response metadata and bodies in checksummed snapshots. WebSocket TLS runs in the renderer with BoringSSL and the UEFI CA variable. IndexedDB provides bounded transactions, indexes, cursors and persistent structured values including Blob/File data. Video-only WebM playback connects the Chromium parser, libvpx VP8/VP9 decoder and software video painting to Blink. URL playback buffers the complete clip before playback and limits encoded data to 8 MiB and retained frame allocations to 64 MiB per player. MSE now connects original Chromium SourceBuffers and ChunkDemuxer to incremental VP8/VP9 decoding, with frame recycling, buffering recovery and seeking. MSE also decodes mono/stereo Opus, synchronizes video to the AC’97 device clock and refills a bounded DMA ring. Separate audio/video SourceBuffers and audio-only MSE are supported. The isolated `build/audio-reservoir` candidate retains PCM in a BSP-owned software queue and refills hardware descriptors while renderer JavaScript is blocked. Its local 1.5-second blocking-script test passes host-timed PCM signal checks. The audio-reservoir live YouTube run completes pause/resume/seek and a visible-video interval, but its initial playback rate fails the real-time gate. Sustained playback and audiovisual synchronization remain incomplete. See [audio reservoir validation](validation/AUDIO-RESERVOIR.md); the default image has not been replaced by this candidate. An [experimental independent Opus service](validation/MEDIA-PROCESS.md) now decodes real mono/stereo MSE packets on a separate CPL3 AP through a bounded syscall broker. Local playback, seeking, rate controls and captured PCM checks pass with this path. A bounded [compressed-audio queue](validation/MEDIA-QUEUE.md) now continues decoding and PCM output during a six-second JavaScript block and passes pause/resume/seek checks. The subsequent [configuration-transition candidate](validation/MEDIA-CONFIG-TRANSITION.md) passes all twelve live YouTube playback checks, including a continuously visible 20-second interval at approximately real-time progression. Audiovisual synchronization and physical output remain unverified. Network/demux replenishment and video presentation still depend on the renderer. The service is disabled by default. See [live YouTube investigation](validation/YOUTUBE.md), [MSE validation and limits](validation/MEDIA-SOURCE.md) and [MSE audio validation](validation/MSE-AUDIO.md). PCM WAV audio now reaches a BSP-owned AC’97 output device through a bounded DMA service. Playback position comes from the device, and volume/mute scale the PCM samples. Dedicated classic and module workers now execute through Blink with message transfer, timers, fetch, importScripts and nested workers. Their contexts are cooperatively scheduled within the renderer. Shared/service workers, compressed audio outside the MSE Opus path, Web Audio and persistent cookies remain unimplemented. WebGL 1/2 now execute through ANGLE and WebGPU executes through Dawn on statically linked SwiftShader. GPU canvas frames reach GOP through CPU readback. See [software GPU validation and limits](validation/SOFTWARE-GPU.md). See [worker execution validation and limits](validation/WORKERS.md). See [audio output validation and limits](validation/AUDIO.md). See [media validation and limits](validation/MEDIA.md), [service status](validation/CHROMIUM-PORT-STATUS.md), [IndexedDB validation](validation/INDEXEDDB.md), [Cache Storage and service probes](validation/CACHE-STORAGE.md), [WebSocket and storage validation](validation/WEB-SERVICES.md) and the earlier [runtime validation](validation/NETWORK-RUNTIME.md).

An opt-in [protected Mesa Venus browser](validation/VENUS-BROKER.md) now connects ANGLE and Dawn to the host RTX 5080 through QEMU and virglrenderer. CPL3 WebGL 1/2, WebGPU compute/rendering, canvas output and 32 repeated buffer-coherence cycles pass. The BSP owns Virtio queues and resource grants; device mappings preserve host cache attributes and remain non-executable. Firmware drivers regain device ownership on exit. The default image still uses SwiftShader. The transport supports up to 16 independent contexts with a shared resource budget. [Multi-context validation](validation/VENUS-MULTICONTEXT.md) passes eight WebGPU/WebGL2 navigation cycles and simultaneous iframe WebGPU and WebGL operation with complete context cleanup. An opt-in [Skia Vulkan compositor](validation/GPU-COMPOSITION.md) now composites validated tiles and native UI into double-buffered GPU images and presents them to a patched QEMU display without reading the final image back to the guest CPU. The [direct GPU canvas candidate](validation/GPU-CANVAS-COMPOSITION.md) imports bounded WebGL/WebGPU image capabilities into that compositor without normal canvas presentation readback. Explicit pixel APIs and unsupported export configurations retain CPU readback. Renderer tile rasterization, producer GPU copies and fence waits remain. Device-loss recovery remains unfinished. Earlier [standalone rendering](validation/VENUS-RENDERING.md) and [Mesa runtime](validation/VENUS-RUNTIME.md) evidence is retained separately.

For persistent file testing, use a dedicated FAT block image. The folder-backed QEMU `fat:rw:` drive produced incorrect file contents after guest truncate/delete operations in the recorded test. The same operations passed across two boots with a raw block image:

```sh
python3 tests/fat-image.py build/blink/profile-disk.img \
  --application build/blink/esp/EFI/BOOT/BOOTX64.EFI
QEMU_DISK_IMAGE="$PWD/build/blink/profile-disk.img" ./run-qemu.sh
```

Creation refuses to overwrite an existing disk. The disk contains a copy of the EFI application at creation time; rebuilding the normal ESP directory does not update that copy. Keep the disk for persistence tests and use a new filename when testing a newly built application. Browser preferences still use the separate OVMF variable store. localStorage uses two alternating, checksummed snapshots per storage key on this disk. sessionStorage remains in memory and resets when its tab closes or the renderer restarts. Cookies remain in memory. Disk-write failures retain the localStorage change in memory and log a persistence error; they do not guarantee durability.

## Navigation

**The browser starts with a blank page and an address bar.** Click the address bar or press Ctrl+L, type an HTTP(S) URL and press Enter. Addresses without a scheme use HTTPS. No website is loaded automatically by the default image. The runner opens an SDL window named `UEFI BROWSER`.

QEMU device-input tests verify address entry, URL replacement, page buttons, links, text editing and vertical wheel scrolling. The earlier `zeroday0619.dev` captures are website compatibility evidence rather than the default home screen. See [validation results](validation/RESULTS.md). Complete Chromium compatibility and every site interaction remain unverified.

## CJK input

Press F9 to switch between Latin and Korean two-set input. The toolbar shows `EN` or `KO`. Ctrl+Space also toggles the mode when firmware delivers a space character. Korean composition uses pinned [libhangul](https://github.com/libhangul/libhangul) sources, linked into the EFI payload with built-in keyboard tables. `prepare-libhangul.py` fetches the exact revision from `revisions.json` and preserves the upstream source.

The address bar and Blink editable fields maintain separate input contexts. Backspace removes the last composition step. Enter confirms pending text; a subsequent Enter performs the editor's normal action. Escape cancels pending text. Pointer focus changes and browser shortcuts finish composition. Address-bar preedit is underlined and uses embedded CJK font fallback. Blink receives composition through `WebInputMethodController`, including composition and composing input events. Text editing still waits for the renderer while it executes long JavaScript tasks.

`build-blink.sh` includes the pinned Mozc engine and its OSS dictionary by default; set `ENABLE_MOZC=0` to omit Japanese input. Lower-level library builds enable it with `build-blink-libraries.py --mozc`. Shift+F9 selects Japanese romaji input. Space requests kana-to-kanji conversion; another Space opens the candidate list. Arrow keys change the selection, and a mouse click selects a displayed candidate. Enter confirms composition and Escape cancels it. The native address bar and Blink share this input adapter. Firmware runtime verification is tracked separately from host conversion tests.

See [CJK input verification](validation/CJK-INPUT.md) and [libhangul notices](licenses/libhangul-COPYING). Hanja conversion and Korean keyboard-layout selection are not connected.

## Tabs and process monitor

Use the `+` button or Ctrl+T to open a blank tab. Click a tab or use Ctrl+Tab / Ctrl+Shift+Tab to switch. Ctrl+1 through Ctrl+4 select a position; Ctrl+9 selects the last tab. Close a tab with its `x` button or Ctrl+W. Closing the last tab creates a new blank tab.

Each tab owns a Blink frame, page pixels, address edit state and session history. Switching preserves the document, form values, JavaScript state and scroll position. Background tabs are marked hidden and stop scheduling paint/raster work. They are not suspended processes: background JavaScript and networking can still run. The browser supports at most four tabs and refuses new tabs when the tracked C heap exceeds 96 MiB. These limits do not guarantee that arbitrary heavy pages will fit in memory. Tabs are not persisted across browser sessions.

Click **Processes** in the tab strip or press Shift+Esc to open the process monitor. Escape or the same shortcut closes it. The native monitor distinguishes the BSP UI/network service from the AP renderer and displays the browser heap and per-tab URL, navigation and presentation state. Historical transfer-worker counters remain available for the local-renderer path. Its refresh target is 500 ms; firmware service calls can still delay updates.

Tracked C heap size excludes virtual-memory commitments and AP page allocations. CPU percentages and per-tab memory are not measured. Tabs share one AP renderer when available. The default CPL3 build requires an enabled application processor and rejects single-CPU or unsupported-firmware configurations. An explicit `ENABLE_RENDERER_CPL3=0` build retains the legacy privileged renderer. The monitor can terminate and restart the shared AP renderer; this discards every renderer document. Closing an individual tab queues its destruction until the current task returns. Opening the monitor hides the selected page and pauses its painting until the monitor closes.

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

The resource loader supports buffered GET, HEAD, POST, PUT, PATCH, DELETE and OPTIONS, CORS preflight and response checks, bounded redirects and first-party credentials. Cross-origin no-cors resources pass the pinned Chromium ORB and CORP checks before delivery to Blink consumers. Document COEP require-corp and credentialless policies apply to subresources and are inherited by workers; see [response-policy validation](validation/RESPONSE-POLICY.md). Streaming/file uploads, keepalive and synchronous main-thread requests remain unsupported. Media range reuse and complete Fetch policy conformance remain unverified. GET/POST navigation preserves buffered form data and redirect method semantics. A pinned EDK II HTTP driver yields at DNS, TCP and TLS wait points. A bounded cooperative network queue keeps each active firmware call on a guarded stack and returns control to the BSP UI loop between waits. The AP submits jobs and retrieves completed responses through a service channel; document callbacks run in its renderer loop. HTTPS/TLS processing remains in the firmware network stack on the BSP. Requests overlap through a cooperative EEVDF queue on the BSP. Eligible requests are selected by weighted virtual deadlines, with a 500 microsecond service target and 4:1 main-document/subresource weights. Firmware calls still yield cooperatively; the target does not preempt long firmware or Blink work. Requests are not executed on firmware application processors. Frame presentation is demand-driven and paced at a minimum 16 ms interval; static pages no longer repaint continuously. Animation callbacks, lifecycle/paint recording and raster work use separate scheduled tasks. Raster tasks replay an immutable Chromium PaintRecord into a staging surface in bounded-height strips; only a complete frame reaches GOP. Input can run between strips, and a document commit discards the previous document snapshot. HTML parsing retains a 4 ms cooperative budget on UEFI. Individual JavaScript calls, lifecycle updates and individual drawing operations still run to completion. Worker-dependent parsing optimizations are disabled in the entry point; general worker-thread support remains incomplete. General browser services and complete response-policy handling remain incomplete. Mouse movement, left/right clicks and keyboard events reach the Blink widget. Visible tooltips, zoom and autoscroll remain incomplete.

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

`BLINK_SOURCE_POSITIONS=eager` adds V8's `--no-enable-lazy-source-positions` flag for comparison builds. It generates source positions during compilation instead of deferring their collection until debugging requires them. The default remains `lazy`; startup cost, memory use and debugger-entry latency must be compared before changing that default.

TurboFan reduces repeated computation cost but does not preempt arbitrary JavaScript; optimization itself is synchronous. Page callbacks and microtask checkpoints retain JavaScript run-to-completion semantics. Firmware cursor capture continues while the renderer is busy, but DOM dispatch can still wait for a long synchronous script or lifecycle operation. The controlled QEMU workload produced matching checksums and reduced the median computation from 9 ms to below the fixture's 1 ms resolution. Input latency did not improve uniformly. See [renderer validation](validation/RENDERER-INPUT-IDENTITY.md) for measurements and limits.

For long-task diagnostics, build an isolated image with
`BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/media-task" ./build-musl.sh`.
The observer reports tasks lasting at least 50 ms with their posting location,
queue delay when available and nested-loop depth. It also records slow Inspector
command phases and JIT gate costs; see [Inspector cost validation](validation/INSPECTOR-COST.md). The option defaults to `0`;
ordinary builds do not register the observer. These wall-time diagnostics can
include debugger waits and add measurement overhead. See
[scheduler task diagnostics](validation/MEDIA-TASK-DIAGNOSTICS.md).

## Run an existing image

```sh
./run-qemu.sh
```

The default uses four virtual CPUs (the default CPL3 build requires at least two enabled CPUs) and opens `build/blink/esp/EFI/BOOT/BOOTX64.EFI` in an SDL window named `UEFI BROWSER`. It tries KVM first and falls back to TCG if KVM cannot initialize. Set `QEMU_ACCEL=tcg` or `QEMU_ACCEL=kvm` to require one backend. The automated browser tests default to TCG; `QEMU_TEST_ACCEL=kvm` selects KVM explicitly. If this image is missing, the runner builds it with `build-blink.sh`. The default display requires QEMU SDL support. VNC is optional.

The default image requires 4 GiB of guest RAM. Press F12 to end the browser session, then press a key to return to firmware. Mouse clicks, Tab, Enter, Space, text editing, PageDown and Ctrl+Home reach the page. UEFI timer capture queues pointer transitions while Blink paints. Ordinary links navigate the active tab. User-activated GET requests for new windows, including target=_blank links, open a new tab within the tab limit. New-window requests open foreground tabs. Independent windows and opener/WindowProxy semantics are not implemented. The EFI image embeds a patched EDK II VirtioInputDxe driver and reconnects existing virtio relative-pointer devices. The driver polls every 5 ms and queues complete virtio reports instead of overwriting button edges. A full report queue applies backpressure to the device ring. Eligible root and nested vertical wheel input uses the BSP scene compositor. Other wheel events reach DOM listeners first; uncancelled events scroll the hit-tested Blink scroll chain, including nested elements. Composited scrolling uses 80 CSS pixels per wheel tick. Renderer-path wheel events request two lines; horizontal wheels, high-resolution touchpad gestures and kinetic scrolling are not implemented. The text path uses firmware UTF-16 keystrokes and synthesized releases. Korean composition uses libhangul and Japanese conversion uses Mozc through the editor composition interface. Physical key-up tracking and held-modifier mouse gestures such as Ctrl+click are not implemented. Keyboard shortcuts use the modifier state attached to each firmware keystroke.

Select another image with `BUILD_DIRECTORY`. The runner does not silently substitute an image when that directory lacks an EFI executable.

The default image embeds the original Chromium DevTools frontend. Click **DevTools** beside the address bar to inspect the active page. F10 and Ctrl+Shift+I remain available as keyboard shortcuts. The button reuses an existing inspector even at the four-tab limit and is disabled inside an inspector tab or when another inspector cannot be created. Inside the tools tab, F8 resumes paused JavaScript and F10 steps over a statement. Console execution, DOM/CSS inspection and mutation, request tracing and debugger pause/step/resume have passed QEMU integration checks. Browser-level domains and several frontend workflows remain incomplete. See [developer-tools validation](validation/DEVELOPER-TOOLS.md) for the tested image and remaining scope.

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

Firmware memory operations currently require four-level x86-64 paging and a free lower-half PML4 slot. Five-level paging is rejected. The runtime provides bounded IPv4 TCP sockets and boot-device files rooted at `/profile/`, brokered by the BSP. These are partial syscall services, not complete POSIX or browser storage services. There is no general executable-process loader. The bounded cooperative thread runtime supports dedicated workers; it does not provide preemptive OS threads. The isolated presentation workers execute a fixed program. Physical hardware and broad web compatibility remain unverified. Human review and validation are required before production acceptance.

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

The default CPL3 path uses a dedicated shared framebuffer and a bounded scroll mailbox. The BSP copies each scroll message into private memory before validating it. The cache contains value metadata and compressed pixels; renderer-owned C++ objects and Skia picture streams never enter the BSP decoder. The explicit `ENABLE_RENDERER_CPL3=0` compatibility build retains the older scene-object path. See [scroll transport validation](validation/SCROLL-WIRE.md).

Native input crosses a pointer-free, versioned message boundary defined in `src/process/renderer_protocol.h`. Messages carry document generations and sequence numbers. A navigation commit discards input queued for the previous document. In multi-CPU mode the native UI first sends input through an acquire/release command ring to the AP renderer. Within that address space, input waits in a bounded queue serviced through Blink's input task runner. This gives DOM event handlers a scheduler task boundary, which Canvas task observers and microtask completion require. Direct dispatch outside a task caused a Canvas assertion during native-click video readback. UEFI's scheduler executes one Blink task per firmware pump iteration, so task batching cannot skip native input polls between scheduled phases.

Main-thread input scroll animation is disabled. Dispatched wheel and keyboard scrolling apply without waiting for scroll animation frames. In the controlled workload, wheel-event response decreased from a 109.7 ms median to 26.8 ms; this changes the input scrolling behavior and is not a general rendering throughput claim.

Frame updates return to the message pump between layout and paint recording. Recording revalidates the complete lifecycle because input or scripts may have changed the document in between. Individual layout operations and JavaScript calls still run to completion.

`src/blink/browser_broker.h` connects each frame's browser interface broker to a local Mojo permission service. Unsupported permission queries, requests and revocations return `denied`; embedded permission controls return unsupported. This prevents unresolved permission promises from aborting Blink during garbage collection. It does not implement device access or permission prompts.

First-party `document.cookie` and HTTP cookie headers use a bounded renderer-session jar with Chromium canonical parsing and access checks. HttpOnly cookies remain inaccessible to script; Secure, domain/path matching, expiry, prefixes and fetch credentials are enforced by the connected paths. Storage is in memory and is lost on renderer restart. Partitioned cookies, nested-navigation cookies and persistent storage are not implemented. Main GET navigation uses a conservative Lax SameSite context. See [cookie policy and Chromium port status](validation/CHROMIUM-PORT-STATUS.md) for the precise limits.

The loader copies the payload's writable ELF segment and TLS template for the AP, remaps the data segment in a private page-table root, and invokes a fresh C/C++ runtime on a separate stack. Blink/V8, layout and normal viewport rasterization execute there. The renderer also rasterizes scroll tiles. Validated tile decompression, composition, native tabs, toolbar, cursor and GOP presentation run on the BSP. Firmware allocations, time, entropy, preferences and HTTP/HTTPS jobs are brokered on the BSP. The local permission-denial broker runs inside each renderer frame.

Renderer allocations reuse private small-object slabs and 1 MiB physical-page batches to reduce service round trips. Acknowledged frame publications prevent concurrent reads of partially painted pixels. Input commands reserve capacity for browser controls, coalesce ordinary pointer motion, and preserve up to 8,192 deferred page-input samples per tab; overflow is counted explicitly. Navigation discards deferred input for the previous document. Closing waits for renderer acknowledgement before reusing the frame slot. Shutdown services outstanding runtime/network requests and allows two seconds for a graceful renderer return before requesting an NMI. The MP completion event must confirm that the AP returned before its stack, TLS and page tables are freed.

The BSP composes eligible scrolling from validated scroll scenes while the renderer runs JavaScript. Scenes preserve transform, rounded-clip, opacity and blend-group ancestry, scroll element IDs, scroll-chain containment and Chromium sticky-position constraints. In CPL3 mode the renderer retains its own image and font resources and rasterizes 512-by-256 pixel tiles. Chromium's existing zlib compresses their pixels. The BSP validates the complete message, including every compressed stream, and retains compressed tiles with private metadata. Visible tiles expand on demand into a cache limited to 32 MiB and 1,024 entries. Recorded offscreen content remains available after visible tiles are evicted. No font, image codec or Skia picture deserializer runs on received scroll data. Publications exceeding the wire, decoded-pixel, record or heap-headroom budgets use renderer dispatch. See [wire format, limits and QEMU verification](validation/SCROLL-WIRE.md).

Native scrollbar parts carry eight cached appearance variants. CSS scrollbars carry geometry and paint profiles for each hovered/pressed part pair, including no part, with one profile for a disabled control. A detached Blink scrollbar resolves pseudo-element styles and positions its parts without changing the live control. Button lengths, thumb minimum lengths, track margins, min/max constraints and part visibility update together on the BSP while the renderer is busy. Hit testing, repeat, thumb travel, picture bounds and clipping use the selected geometry. Drag capture compensates for geometry changes so they do not introduce an extra scroll jump. Pointer departure into the toolbar clears hover, and native arrows become disabled at scroll boundaries. Disabled CSS scrollbars reject input. Identical variants share serialized and decoded data, and tile keys include the selected profile.

Custom scrollbar outer thickness changes use cached Blink reflows of the last published DOM, including size-container-query layout, bounded `scroll-state(scrollable)` and `scroll-state(stuck)` states, and local iframe contents. The renderer calculates each supported width/height combination with real text wrapping, Grid/Flex layout, scroll ranges, clips and sticky constraints. The BSP switches the complete scene while JavaScript is running, preserves absolute scroll offsets and sends the corresponding gutter states before subsequent page input. Blink applies the layout before hit testing queued content clicks. The live DOM and its observers update when the renderer resumes. Speculative capture suppresses script, observer delivery, intermediate viewport/scroll events and scroll anchoring; it restores the original geometry and offsets before returning.

Reflow caching permits up to 64 combined thickness/query states and 16 MiB of serialized data and geometry, with positive thicknesses no larger than 256 pixels. Native `scrollbar-width` changes, host-element hover rules, `snapped`/`scrolled` queries, mixed stuck/scrollable dependencies on one container, anchored queries, remote frames, removal or replacement of a driving scrollbar, geometry-dependent changes to the scrollbar thickness rules, unsupported paint properties and larger caches retain renderer dispatch. Dependent scrollbars may appear, disappear or acquire a new layout object. DOM identities connect surviving scroll areas across scenes; removed areas lose their scroll position. Alternate scenes share BSP font/image resources when their resource keys match and retain only the active scene's raster tiles. Custom track pieces still use full-track paint records clipped at the moving thumb midpoint, so gradients and borders tied to a changing track-piece size may differ during asynchronous motion. Local frame trees are limited to 16 frames. Child frames share the renderer and software widget; HTTP(S) GET/POST, `about:blank` and `srcdoc` loads preserve frame policy and origin handling. See [scroll-state and topology validation](validation/SCROLL-STATE.md), [container-query and iframe validation](validation/GUTTER-TREE.md), [gutter reflow validation](validation/SCROLL-GUTTER.md), [scrollbar appearance validation](validation/SCROLL-STYLE.md) and [scrollbar part layout validation](validation/SCROLL-LAYOUT.md).

Absolute scroll commands identify the target scroll container and carry sequence acknowledgements. Nested regions scroll independently, boundary input can propagate through scroll ancestry, and `overscroll-behavior: contain` prevents propagation. Sticky constraints update visually before Blink processes the queued offsets. Navigation and recovery discard scene resources. Passive wheel listeners receive non-cancelable notifications after asynchronous scrolling. Blink blocking-wheel hit-test regions retain cancelable renderer dispatch; unrelated regions remain eligible. Arrow, PageUp/Down, Home/End and Space scrolling use the acknowledged focused scroll container when its keyboard event path has no listeners or editing/activation defaults. Native and CSS `::-webkit-scrollbar` controls are painted separately for both axes. Thumb dragging, arrow buttons, track paging and held-button repeat update the BSP scene while the renderer is busy. ArrowLeft/Right scroll eligible focused containers horizontally. Absolute commands carry both coordinates, including negative RTL offsets. Non-translation transforms, path clips, filters, scroll snapping and repaint-dependent backgrounds still use renderer dispatch. Cancelable wheel/keyboard handlers, text editing, JavaScript-built scrollbar widgets and scroll-linked JavaScript still wait for the renderer. Lazy or newly changed content requires a new renderer scene. In CPL3 mode the BSP decompresses and composites visible 512×256 pixel tiles; this is not a worst-case latency guarantee or Chromium's complete compositor. See [nested scroll and tile validation](validation/SCROLL-TREE.md) [asynchronous input validation](validation/SCROLL-INPUT.md) and [two-axis scrollbar validation](validation/SCROLL-CONTROLS.md).

The entropy broker uses a bounded shared bounce buffer before copying into renderer memory. Firmware must not write directly to an AP-only virtual mapping. Both the private-mapping runtime probe and a 65,536-byte JavaScript `crypto.getRandomValues()` regression cover this boundary.

Open **Processes** or press **Shift+Esc**, then click **End renderer** or press **Delete** to interrupt the AP, including infinite JavaScript. The loader uses an NMI and a private exception table to return to its supervisor. Recoverable divide, invalid-opcode, general-protection and page-fault exceptions use the same cleanup path. Runtime aborts in the AP raise an invalid-opcode exception. The BSP waits for AP quiescence and MP completion, abandons outstanding renderer network jobs, releases retained responses and broker allocations, and starts a fresh renderer. Active firmware requests keep their stacks until completion. Native tabs and address text remain, but all renderer documents, JavaScript state and per-tab session histories are discarded. Reload a tab or enter a URL to restore a document. Failed restart attempts leave the native UI available for another attempt.

The default CPL3 build protects supervisor memory and page tables and uses separate syscall and exception stacks. QEMU tests cover supervisor read denial, page-table write denial and interactive recovery while a real Blink document is open. This is an experimental renderer privilege boundary, not Chromium's complete site-isolation or sandbox architecture. Firmware, BSP services and CPU behavior remain trusted. See [current CPL3 validation](validation/RENDERER-CPL3.md); the earlier [scroll and recovery validation](validation/SCROLL-RECOVERY.md) and [AP renderer validation](validation/RENDERER-PROCESS.md) describe the legacy CPL0 path.
