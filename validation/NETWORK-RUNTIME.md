# Network and firmware runtime service validation

The GPU availability statements below describe this report's recorded build. See [software GPU validation](SOFTWARE-GPU.md) for the subsequent ANGLE, Dawn and SwiftShader implementation.

This report records the earlier TCP/file implementation. WebSocket, WSS, DNS, shared-memory data pipes and Web Storage were connected subsequently; see [WebSocket and storage validation](WEB-SERVICES.md) for their current status. Remaining-work statements below describe the image hashes recorded in this report.
## Scope

This change adds buffered HTTP methods, CORS and redirects to the Blink loader, plus BSP-owned TCP and profile-file services to the musl syscall bridge. It does not complete the Chromium browser port. WebSocket, worker hosts, browser storage, media devices, GPU services and a hostile-renderer sandbox remain absent.

## Implemented behavior

- HTTP GET, HEAD, POST, PUT, PATCH, DELETE and OPTIONS use the cooperative firmware queue. Request and response bodies are bounded at 8 MiB. The EDK II HTTP patch now accepts and serializes OPTIONS.
- The Blink loader uses Chromium CORS access and preflight helpers. It checks allowed origins, methods, headers and credentials; exposes permitted headers; and supplies opaque no-cors fetch results. Redirects are limited to 20, preserve or rewrite methods as required by their status, remove cross-origin Authorization and use Chromium redirect/referrer calculations.
- Main and local-frame navigation accept buffered GET/POST requests. POST forms preserve their bodies through 307/308 redirects. New-window POST and POST replay through history/reload remain unsupported.
- `src/network/sockets.c` implements IPv4 TCP connection, nonblocking send/receive, readiness polling, EOF, address queries, descriptor flags and abortive close. The BSP owns all EFI TCP4 protocol objects and token buffers. The service has 32 descriptors with 64 KiB transmit and receive buffers per descriptor.
- `src/musl/files.c` confines its pathname API to `/profile/` on the application boot device, represented by `EFI/UEFIBrowser/Profile`. It implements a bounded open/read/write/seek/truncate/flush/stat/delete/mkdir subset with UTF-8 pathname conversion and traversal rejection. It has 64 descriptors and limits individual transfers to 1 MiB.
- File and socket descriptors carry browser/renderer ownership. Renderer requests cross the existing service channel; firmware protocols are called on the BSP. Renderer reset releases its descriptors.
- Chromium's Linux ScopedFD wrapper uses the firmware close syscall under `UEFI_RUNTIME`. The regular dynamic `RTLD_NEXT` lookup cannot resolve close in this statically linked runtime.

## Errors reproduced and corrected

The first socket run failed EFI TCP4 configuration because SACK and path-MTU options were requested from a backend that does not support them. Those optional flags are disabled. Receive completion also exposed incorrect FIN/RESET status mapping; FIN now produces EOF and RESET maps to connection reset. A static close-symbol lookup aborted the renderer I/O probe; the UEFI-specific close adapter fixes that path.

The folder-backed QEMU `fat:rw:` disk passed immediate guest reads but returned a two-byte renderer file after shutdown and retained a deleted transient file. Repeating the same application and operations on a real FAT16 block image preserved both four-byte counters and removed the transient file. Both counters incremented from 1 to 2 on the second boot. See [directory-backend bytes](io-directory-results.json) and [block-disk comparison](io-block-results.json). This comparison isolates the observed mismatch to the folder-backed disk path; it is not a comprehensive diagnosis of QEMU vvfat internals.

`QEMU_DISK_IMAGE` selects a raw disk in `run-qemu.sh`. `tests/fat-image.py` creates a dedicated test disk without mounting it and refuses to overwrite existing files. Disk images contain an EFI copy taken at creation time and are not updated by later ESP builds.

The full regression run also exposed an uninitialized Blink network-state notifier when the live site read network information. Both AP and local-renderer initialization now set online state, unknown connection type and default data-saving state before any frame is created. The online flag is optimistic because link-change detection is not connected; it is not a reachability measurement. A deterministic `navigator.onLine` / `navigator.connection.downlink` assertion now covers this crash.

## Verification

Environment: Chromium 158.0.8110.0 (`f12c350cb1e1e7ea05c11d6045b0a04158ec9e28`), musl 1.2.5, Debian clang 23.1.3, QEMU 11.1.2 and OVMF 2026.08+ds-2, x86-64 KVM, four vCPUs and 4 GiB RAM per VM.

The normal EFI image SHA-256 is `3cd095451f7bbb6c612f2866e6acebd158c5606ddad211ef2f94dc3f7323665f`. The separate image with the native I/O probe enabled is `2dc22ec837338b8f238597894039be6701ecb681afa4d57b4f6a750200f309ba`.

- HTTP and HTTPS each passed 34 JavaScript assertions. Tests include binary request bodies, method handling, allowed/denied CORS, preflight rejection without sending the denied request, credentials, opaque responses and redirects. Cross-origin Authorization stripping is implemented but is not asserted by these 34 cases. QMP then clicked a real POST form; the server received the expected encoded Korean text through a 307 redirect. Logs and screenshots are `runtime-final-network*`. [Fixture request records](runtime-network-wire.log) include both initial/final network runs and native TCP probes; they are not a per-run packet capture.
- `tests/runtime-io.py` passed both raw-disk boots. The browser and AP renderer each completed a nonblocking TCP exchange containing NUL and `0xff`, readiness/EOF checks, file write/flush/reopen/truncate/delete and path rejection. Renderer access to browser-owned descriptors returned EBADF. Host-side FAT reads independently confirmed both four-byte counters and the absence of the deleted file. See [machine-readable results](runtime-io-final-results.json) and `runtime-io-final-{1,2}.log`.
- The normal and probe EFI builds passed strict static linkage. Build logs are [production](runtime-production-build.log) and [probe](runtime-probe-build.log). Existing musl `endian.h` shift-precedence warnings remain.
- The final production image passed `BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py`: static EFI/ELF format, bounded load segments, no required unresolved symbols, no PE imports and no host syscall instructions. See [binary validation](runtime-final-binary.log). Optional weak hooks remain unresolved by design.
- Chromium and EDK II patch reverse-application checks passed against the source trees used for the build. Python syntax, shell syntax and `git diff --check` checks passed.

The [initial 22-case regression](runtime-release-suite.json) passed 21 cases and failed `tabs-site` on the network-state DCHECK. That manifest and crash log are retained. The final image passed all seven cases in [the targeted suite](runtime-final-suite.json); the initial 21 passes must not be attributed to the final image. The live site now loads and passes its tab/interaction scenario; the [captured interactive page](runtime-final-tabs-site-input-tabs-site-interactive.png) was visually inspected. [Single-CPU network validation](runtime-local-network-results.json) also passed all 34 assertions and the POST form with clean firmware return. The full 22-case suite was not repeated after the network-state initialization fix. The normal-image suite covers network, secure network, cookies, secure cookies, scroll-state, sticky state, local iframe state, scrollbar topology/replacement, nested frame reflow, root/horizontal gutters, scrollbar layout/controls/style, keyboard and pointer scrolling, scroll trees, real-site tabs and frame policy. These fixtures are not the Chromium browser suite or Web Platform Tests and do not establish arbitrary website compatibility.

## Reproduction

Start the local fixture servers in a separate terminal. The generated test CA is restricted to the test VM and does not change the production CA bundle:

```sh
python3 tests/network-servers.py
```

Build and test the ordinary image:

```sh
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
python3 tests/state-suite.py --https-ca build/network-https/ca.esl \
  --prefix runtime-check network network-secure
```

Run all 22 ordinary-image regressions by omitting the case list:

```sh
python3 tests/state-suite.py --https-ca build/network-https/ca.esl --prefix runtime-check-all
```

Build and test the native service probe:

```sh
ENABLE_BLINK=1 ENABLE_SERVICE_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/services-release" ./build-musl.sh
python3 tests/runtime-io.py --build build/services-release --prefix runtime-io-check
```

Use a new prefix for repeated I/O runs to retain earlier disk evidence. The fixture services must remain running during both boots.

## Unimplemented services and boundaries

TCP currently excludes IPv6, UDP, server sockets, DNS, socket TLS, epoll, graceful shutdown and most socket options. Close cancels pending firmware transfers; callers cannot rely on it to flush queued sends. There is no WebSocketConnector implementation, so JavaScript WebSocket is not enabled. HTTP TLS still runs through the existing firmware HTTP service.

The file API does not implement SQLite/IndexedDB, Cache Storage, localStorage, persistent cookies, file mapping, directory enumeration, atomic rename or complete POSIX semantics. Quota-managed browser storage remains separate work.

Worker host interfaces, general worker threads, service workers, audio/video playback and capture, WebRTC, WebGL/WebGPU providers and GPU-process services are not connected. Existing Skia software rendering is not a GPU backend.

The renderer executes at CPL0 with firmware mappings and trusted request pointers. Descriptor owner checks prevent API misuse in the tested cooperative runtime; they do not confine malicious native code. A security sandbox requires enforced privilege separation, restricted mappings and validated IPC buffers, none of which is supplied by these changes.

Cross-origin no-cors subresources remain blocked pending ORB/CORP integration. Streaming and file uploads, keepalive, synchronous resource loading and complete navigation/referrer/response policy are not implemented. The [Fetch standard](https://fetch.spec.whatwg.org/) defines the broader semantics; local fixture success is narrower than conformance.
