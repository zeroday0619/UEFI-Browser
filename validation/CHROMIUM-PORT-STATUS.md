# Chromium port status

This project embeds selected Chromium 158.0.8110.0 components in a UEFI runtime. It does not run the complete Chrome browser application. Linking Blink, V8, Skia and their dependencies does not connect every web API to its required browser, operating-system, storage or device service.

## Implemented paths and limits

| Area | Current implementation | Remaining work |
| --- | --- | --- |
| Rendering and input | Blink DOM/CSS/layout, V8 JavaScript, Skia software painting and cached asynchronous scrolling. | Complete Chromium compositor behavior, arbitrary paint effects, GPU acceleration and broad conformance testing. |
| Scroll-state queries | Cached `scrollable` and `stuck` layout states within the bounded scene cache. | `snapped`, `scrolled`, combined stuck/scrollable dependencies on one container, anchored queries and larger state spaces use renderer dispatch. |
| Navigation | HTTP(S) GET and buffered POST, redirect method handling, top-level history, local child-frame navigation, `about:blank` and `srcdoc`. | POST replay in history/reload, new-window POST, complete child history, out-of-process frames and complete browser navigation policy. |
| Fetch and subresources | Buffered GET/HEAD/POST/PUT/PATCH/DELETE/OPTIONS, CORS preflight and response checks, opaque no-cors fetch, bounded redirects and 8 MiB body limits. | Cross-origin no-cors subresources with ORB/CORP enforcement, complete Fetch policy, streaming/file uploads, keepalive and synchronous requests. |
| Runtime TCP | BSP-owned IPv4 TCP descriptors with nonblocking connect, send/receive, poll, EOF and owner checks. | IPv6, generic UDP, listen/accept, general socket TLS, graceful TCP shutdown and complete socket compatibility. DNS and TLS are connected to the WebSocket path. |
| Runtime files | BSP-owned boot-device `/profile/` files with read/write, seek, truncate, flush, deletion and owner checks; raw FAT disk persistence verified. | Atomic replacement, directory enumeration, ordinary file mapping and complete POSIX behavior. |
| WebSocket | Blink Mojo connector, IPv4 TCP/DNS, verified WSS, text/binary frames, fragmentation, ping/pong and close handshake. | Compression, redirects, HTTP authentication, proxies, IPv6, complete policy/conformance coverage and execution independent of renderer JavaScript. |
| Web Storage | Origin/partition-keyed localStorage snapshots, per-tab sessionStorage, Blink cache and storage events. | Storage management UI, origin eviction, crash/power-loss guarantees, complete opener-clone integration and WPT coverage. |
| Cache Storage | Partition-keyed named caches, query matching, response bodies, atomic in-memory batch candidates and two-slot disk snapshots. | Service-worker integration, eviction/quota management, power-loss guarantees, full COEP/CORP policy and WPT coverage. |
| IndexedDB | Partition-keyed databases, serialized read/write and upgrade transactions, indexes, cursors, Blob/File values and two-slot disk snapshots. | Browser-owned service isolation, worker integration, quota/eviction management, power-loss guarantees and WPT coverage. |
| Blob | Bounded immutable bytes, composition/slices, large byte providers and asynchronous Mojo pipe reads. | File-backed blobs, blob URLs, side data and complete Blob/stream conformance. |
| Shared memory | Bounded renderer-local memfd handles, shared physical pages through distinct virtual mappings, read-only mappings and size seals for Mojo data pipes. | General POSIX memfd semantics and descriptor transfer across isolated processes. |
| Dedicated workers | Real Blink classic/module execution, message and ArrayBuffer transfer, timers, HTTP(S) fetch, importScripts, nested workers, termination and bounded capacity recovery. Separate stacks/TLS with cooperative scheduling. [Validation](WORKERS.md). | Preemption, CPU-parallel worker execution, Blob/data scripts, SharedWorker/ServiceWorker, worker storage/socket brokers and full conformance. |
| Audio playback | PCM WAV, mono/stereo, 8/16-bit, 8–48 kHz; BSP-owned AC’97 DMA, device position, play/pause/seek/end, linear software volume and mute. [Actual output validation](AUDIO.md). | Compressed codecs, Web Audio, mixing, streaming, pitch-preserving rate changes, general device drivers and physical-speaker verification. |
| Video playback | HTTP(S) video-only, eight-bit VP8/VP9 WebM; software painting, canvas readback, playback, pause, seek, rate, loop and frame callbacks. Complete clips are buffered within per-player limits. See [media validation](MEDIA.md). | WebM audio tracks, streaming/MSE, encrypted media, other codecs, hardware decoding, full color management and broad conformance. |
| Cookies | Chromium canonical parsing and access checks, document access and HTTP headers in a bounded renderer-session jar. | Persistent storage, partition keys, third-party cookies, Cookie Store API, browser-owned storage, complete SameSite navigation context and WPT coverage. |
| Browser shell | Address bar, history buttons, tabs, settings and process diagnostics. | Chrome profiles, extensions, account integration, download management and the complete Chrome settings surface. |

## Services that are not connected

`src/blink/browser_broker.h` binds WebSocketConnector, CacheStorage, IDBFactory and the permission service. The process broker binds BlobRegistry. Unsupported device permissions are denied. Web Storage binds its area service directly before synchronous cache reads. Other requested browser interfaces remain disconnected. The following are separate implementation tasks, not completed features:

- **WebTransport and WebRTC:** Chromium library availability does not supply the required UDP/TCP transport, media-device services, permissions or capture/playback backends.
- **Workers and offline storage:** Shared/service-worker hosts and browser-managed quota/eviction remain disconnected. Dedicated workers use the limited execution path documented above. Their presence in Blink's JavaScript bindings is not evidence of working storage or execution.
- **Audio and GPU APIs:** Compressed audio codecs, Web Audio, capture devices, hardware graphics contexts, WebGL/WebGPU providers and protected-media services remain disconnected. PCM WAV uses the limited AC’97 output path above.
- **Device and desktop APIs:** Clipboard, file selection, downloads, notifications, USB/Bluetooth/HID/serial access, printing and accessibility integration need platform services and explicit permission handling.
- **Process security:** Current renderer execution uses CPL0 with private image mappings, stack and TLS. This is not Chromium's sandbox or site isolation.

This inventory identifies concrete gaps but is not an exhaustive web-platform conformance result. The Chromium browser test suite and Web Platform Tests have not been run for this port. No claim of complete Chromium compatibility is made.

## Cookie policy in this implementation

The jar contains at most 512 cookies and expires entries against the firmware clock. A cookie line is limited to 4096 bytes and each outgoing Cookie header to 8191 bytes to fit the HTTP transport. Entries live only until the renderer exits or restarts. No cookie file is written.

`document.cookie` is available only to non-opaque HTTP(S) documents whose site-for-cookies is first party. The bridge uses Chromium's `CanonicalCookie`, `CookieOptions` and inclusion-status checks for parsing, domain/path matching, expiry, Secure, HttpOnly and cookie prefixes. Script cannot read or replace an existing HttpOnly cookie. An insecure response cannot overwrite a matching Secure cookie.

Same-origin resource requests attach and store cookies only when their credentials mode permits it and their site-for-cookies is first party. `credentials: "omit"` suppresses both request cookies and response storage. Main-document GET navigation uses a conservative Lax context, so it withholds Strict cookies even when a richer navigation context could permit them. POST attaches cookies only when the supplied requestor origin matches the destination; it then uses a Strict context. Cross-origin POST navigation withholds all cookies. Nested-frame navigation currently excludes cookies. Partitioned cookies are rejected rather than stored without a partition key.

These limits are narrower than a complete browser cookie implementation. Cookie semantics and credentials behavior are specified in [RFC 6265](https://httpwg.org/specs/rfc6265.html) and the [Fetch standard](https://fetch.spec.whatwg.org/#credentials). Passing local integration fixtures does not establish full conformance.

## Network and runtime validation

See [Cache Storage and service probes](CACHE-STORAGE.md) for the latest changes and remaining service failures. [WebSocket and storage validation](WEB-SERVICES.md) records the preceding implementation and tests. The earlier [network and runtime report](NETWORK-RUNTIME.md) records the TCP/file foundation and its original service gaps. Descriptor ownership is an API check, not a hostile-renderer security boundary.
