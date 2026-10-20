# WebSocket and Web Storage integration

Cache Storage and Blob were added in a later change. See [Cache Storage and service probes](CACHE-STORAGE.md) for current evidence; image hashes below identify the preceding release.

Blink's JavaScript WebSocket, localStorage and sessionStorage APIs now connect to UEFI runtime services. This change also supplies the bounded shared-memory backing needed by Mojo data pipes. It does not complete the Chromium port: worker hosts and threads, IndexedDB/Cache Storage, media backends, GPU providers and a hostile-renderer sandbox remain absent.

## Connected implementation

| Component | Files | Behavior |
| --- | --- | --- |
| WebSocket service | `src/blink/web_socket.cc`, `browser_broker.h`, `browser_frame.cc` | Each local frame binds a connector. Connections use nonblocking firmware sockets, Chromium framing/handshake helpers and Mojo producer/consumer pipes. Text, binary, fragmented messages, control frames and close handshakes reach page JavaScript. |
| WSS | `src/blink/web_socket.cc` | BoringSSL verifies certificate chains against the UEFI `TlsCaCertificate` variable and checks the DNS name or IP SAN. DNS connections send SNI. The minimum TLS version is 1.2. Invalid trust and identity checks terminate the connection. |
| DNS | `src/network/dns.c`, `runtime/firmware_io.h`, `src/process/renderer_runtime.c` | BSP-owned DNS4 protocol instances use DHCP/default network configuration. The renderer starts, polls and releases owner-tagged queries through service operation 130. |
| Web Storage | `src/blink/web_storage.cc`, `browser_frame.cc`, `patches/blink-build.patch` | Blink storage keys select areas. Local storage retains Blink cache/event behavior and writes alternating profile snapshots. Session namespaces belong to tabs. Frame-token and storage-key checks reject mismatched or opaque access. |
| Shared memory | `src/musl/shared.c`, `src/musl/syscalls.c`, `src/virtual_memory.c`, `src/posix.c` | A bounded memfd subset supplies duplicate/read-only descriptors, size seals and mappings that alias the same physical pages. Reference counts keep backing alive after descriptor close. Read-only aliases cannot gain write permission through mprotect; shared pages remain non-executable. |

WebSocket tasks perform bounded work between renderer polls. Firmware owns TCP and DNS protocol objects; TLS and WebSocket parsing execute on the renderer task sequence. Long synchronous JavaScript can therefore delay WebSocket progress. This implementation is not a separately scheduled Chromium network process.

WebSocket cookies are conservatively restricted to matching document/request origins and first-party site-for-cookies contexts. Existing cookie parsing and inclusion checks remain in use. The test suite exercises protocol and TLS behavior, not complete WebSocket cookie policy. Redirects, authentication challenges, proxy negotiation, permessage-deflate, IPv6 and full Local Network Access policy are not implemented. RFC 6455 defines the wider protocol requirements; the fixture coverage is narrower than conformance: [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455).

## Storage format and limits

Each localStorage area uses the serialized Chromium storage key as its identity. The filename contains SHA-256 of that identity. Each snapshot contains a format marker, generation, identity, key/value lengths and bytes, followed by a SHA-256 checksum. Loading validates both slots and selects the valid snapshot with the highest generation. Writing replaces the alternate slot and flushes it before advancing the in-memory generation.

This preserves an earlier valid application snapshot when the latest payload is corrupt. It does not guarantee FAT metadata atomicity or physical power-loss recovery. A write failure logs a persistence error and retains the change in renderer memory. Filesystem errors do not currently roll back Blink's cache or raise a synchronous JavaScript exception.

The backend allows 64 combined storage areas, 16,384 entries per area and Chromium's per-area quota plus its backend allowance. Session areas are removed when their namespace is released. Local areas remain cached until renderer exit; origin eviction and storage-management UI remain work. A namespace clone implementation exists, but complete opener/window creation and clone behavior have not been verified.

WebSocket limits are 16 connections per frame, 8 MiB per message/queue and 1,024 queued messages. The underlying runtime permits 32 TCP sockets and 16 DNS queries. Shared-memory backing is limited to 64 MiB per runtime, 8 MiB per region, 256 handles and 512 mappings. It supports the operations used by the connected Mojo path, not arbitrary memfd I/O, resizing after allocation, partial unmapping or cross-process descriptor transfer.

## Failures found and repaired

- Sending localStorage-only observer configuration to a sessionStorage cache triggered `cached_storage_area.cc:498`. Session storage now uses Blink's session event behavior without those observer messages.
- The first WebSocket upgrade reached the server, then Mojo attempted the unavailable `/dev/shm` fallback and aborted. The memfd/mapping implementation now supplies actual shared backing.
- musl adds `O_LARGEFILE` when reopening a read-only memfd handle. Rejecting that harmless flag failed the native shared-memory probe; the accepted flag mask was corrected.
- The first tab-lifecycle fixture assumed Ctrl+R support. No reload occurred because that shortcut is not implemented. The fixture now clicks the existing Reload button.

Initial failures remain in `services-storage-initial-*`, `services-websocket-initial-*`, `services-websocket-shared-*` and `services-final-*` evidence. Build-time API mismatches were corrected before the successful EFI runs.

## Verified results

The final EFI image SHA-256 is `bc4344e0672a24acf358c4cf6d15c40287bb91be69621721850b734073e32f01` (Chromium 158.0.8110.0 and musl 1.2.5). Tests used QEMU 11.1.2 with OVMF and KVM. All final manifests below identify this image.

- [Six QEMU/KVM cases](services-release-suite.json) passed: WebSocket, Web Storage, reload/tab storage lifecycle, secure fetch, frame policy and live-site tabs. The WebSocket page passed 17 assertions. The storage page passed 14 assertions; its lifecycle case repeated the page across reloads and independent tabs.
- [Single-CPU fallback](services-single-cpu-suite.json) passed the same WebSocket and storage fixtures without the AP renderer.
- [Three raw-disk boots](services-release-persistence-results.json) passed all 14 storage assertions on each boot. The page's local counter advanced 1, 2, 3 while its session counter remained 1. Host-side FAT reads verified snapshot checksums. Corrupting the newest snapshot at generation 11 caused the third boot to load generation 10 and continue normally.
- Every final run passed the native shared-memory probe: alias visibility, read-only page permissions, rejected write/execute escalation, size seals, descriptor closure with live mappings and shared-page discard rejection. AP runs exercised the probe in both browser and renderer runtimes.
- The final [WebSocket screenshot](services-release-websocket-input-websocket-checks.png) and [zeroday0619.dev interactive screenshot](services-release-tabs-site-input-tabs-site-interactive.png) were inspected. The site test also exercised native tab/settings transitions. This is a regression check of that page, not a visual parity or arbitrary-site claim.
- [EFI binary validation](services-release-binary.log) passed: bounded ELF loads, static x86-64 EFI image, no required unresolved symbols, no PE imports and no host syscall instructions. Optional weak hooks are listed in the log. [Native compilation](services-release-native-build.log) and [EFI linking](services-release-link.log) passed; existing compiler warnings remain.
- Python syntax checks, shell syntax checks, Chromium patch reverse-application checks and `git diff --check` passed.

The [aggregate fixture wire log](services-websocket-wire.log) records Origin/User-Agent headers and masked frame payload hashes across the debugging and final runs. It is not a per-run packet capture. The earlier [nine-case run](services-final-suite.json) passed eight cases and failed the unsupported-shortcut fixture; its image predates the final WebSocket cookie-policy adjustment. Those eight passes are not attributed to the final image. The final six-case suite replaced the failing fixture and rechecked affected behavior; the full default 25-case suite was not run on the final image.

## Reproduction

Start both fixture services in separate terminals. They bind localhost only. The WebSocket TLS service uses the certificate generated by the HTTP fixture, so start the HTTP fixture first and keep both running:

```sh
python3 tests/network-servers.py
python3 tests/websocket-server.py
```

Build and run targeted QEMU tests:

```sh
python3 build-blink-libraries.py --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
python3 tests/state-suite.py --https-ca build/network-https/ca.esl \
  --prefix web-services-check websocket storage storage-lifecycle \
  network-secure frame-policy tabs-site
python3 tests/storage-persistence.py --prefix storage-check
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

The persistence test creates an exclusive raw FAT disk, boots it three times, checks stored files independently on the host and corrupts only the latest snapshot in that dedicated test disk before the third boot. Use a new prefix for each run. Folder-backed `fat:rw:` disks are unsuitable for this persistence test because truncate/delete behavior failed earlier validation. The raw disk embeds the EFI application copied at creation time.

The DNS fixture name `10.0.2.2.nip.io` resolves to QEMU's host gateway. Those two DNS/hostname tests require working external DNS; all WebSocket application traffic stays on the local fixture. The generated test CA is supplied only to the test VM and does not change the production CA bundle.

## Remaining requested services

Dedicated/shared/service-worker host factories and general native threads are still missing; `src/musl/startup.c` returns ENOTSUP from pthread_create. Media playback/capture has no connected WebMediaPlayer/device pipeline. WebGL/WebGPU have no context provider or GPU device service. Existing Skia CPU painting does not supply those APIs.

The renderer remains at CPL0 with firmware mappings and trusted service pointers. Read-only memfd aliases and descriptor ownership are useful API checks, but do not confine malicious renderer code. A sandbox requires a renderer-compatible unprivileged execution environment, restricted mappings, validated IPC copying and exception/syscall handling. The existing isolated frame-transfer workers do not sandbox Blink or V8.

No full Chromium browser suite, Web Platform Tests or physical-hardware validation was performed. Human review and validation remain required for these AI-assisted changes.
