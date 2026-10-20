# IndexedDB integration

`IDBFactory`, database connections, transactions and cursors now connect Blink to a bounded IndexedDB backend in the UEFI renderer. The implementation stores actual structured values and Blob/File contents in the boot-device profile. It uses Chromium's existing Mojo interfaces and key comparison/serialization types; it does not port Chromium's LevelDB or SQLite storage service.

## Execution and persistence

`src/blink/indexed_db.cc` implements database enumeration/open/delete, schema upgrades, object stores, auto-increment keys, add/put/get/getAll/getAllKeys/getAllRecords, count, delete ranges, clear, indexes and cursors. Transactions execute serially per database against a private candidate. A successful write transaction flushes an alternate snapshot before publishing the candidate. Abort discards the candidate, including schema changes and key-generator changes. Unique-index failures retain existing records. Index population uses a preemptive operation queue so ordinary requests and commit cannot overtake it.

Cursors recompute their ordered view for each operation, including changes made earlier in their transaction. Forward/reverse iteration, key ranges, unique directions, advance, continue, prefetch and prefetch reset are connected. Bulk results use chunks of at most 1,000 records. Blob reads retain transaction ordering while asynchronous Mojo data-pipe transfers complete. The backend restores blobs as new references and preserves File name/type/time metadata.

The frame broker selects a non-opaque Chromium storage key. Filenames are SHA-256-derived from its serialized identity. Each origin alternates two checksummed snapshots under `/profile/idb-…-0` and `/profile/idb-…-1`. Loading validates the format, identity, checksum, Mojo payloads, keys, collection limits and index consistency before selecting the newest valid generation. The format marker includes Chromium `158.0.8110.0`; schema changes require migration or a new format version.

The snapshot quota is 8 MiB per storage key. There are at most 32 storage keys, 32 database-name slots per key per renderer lifetime, 64 stores per database, 64 indexes per store, 4,096 records per store, 4,096 keys per record/index, 256 external objects per value and 4,096 UTF-16 code units per Blob MIME type. Database names are limited to 4,096 code units. Each database permits 32 connections, 64 queued open/delete requests and 128 transactions; each connection permits 64 transactions. Blob transport imposes its own existing byte and transfer limits. These quotas do not bound every transient renderer allocation.

The backend runs on the renderer event loop. Snapshot encoding, index scans and file writes can delay it. It does not provide a separate browser storage process, worker connections, StorageManager quota/eviction, file-system access tokens, full WPT conformance or power-loss atomicity of FAT metadata. Checksums detect application payload corruption; they do not authenticate the file against an attacker. Current renderer execution remains CPL0 and has no security sandbox.

## Verified results

EFI SHA-256: `250387f964a025c87576acc28896ff811280172077917d718c55a5c2b99be5e0`. The build uses Chromium 158.0.8110.0, musl 1.2.5 and Debian Clang 23.1.3. Runtime tests use QEMU 11.1.2 with OVMF and KVM.

- [Native compilation](idb-final-native-compilation.log), [EFI linking](idb-final-link.log) and [binary validation](idb-final-binary.log) passed. The image has no required unresolved symbols, PE imports or host syscall instructions. Existing upstream compiler warnings remain.
- The [seven-scenario suite](idb-final-suite.json) passed IndexedDB, service diagnostics, Cache Storage, WebSocket, Web Storage, secure network requests and live-site tabs. The diagnostic case requires IndexedDB write/read success; its other service failures are recorded separately.
- [Three independent raw-disk boots](idb-final-recovery-results.json) each passed 37 IndexedDB assertions. A counter advanced 1, 2, 3 and Blob contents survived. Before boot three, the fixture corrupted snapshot generation 59. The guest loaded generation 58 and retained the committed counter and Blob. Host-side FAT reads verified the snapshot checksums.
- The [single-CPU suite](idb-single-suite.json) passed all 37 assertions and the service probes without a separate AP renderer. The final recovery and single-CPU tests include a 90,000-byte pseudorandom array alongside a large string, forcing the value through Blink's Blob wrapping path despite compression.
- [IndexedDB](idb-single-indexeddb-input-indexeddb-checks.png) and [zeroday0619.dev](idb-final-tabs-site-input-tabs-site-interactive.png) screenshots were inspected. The live-site case also exercises native tabs, settings and page interaction. This is not a visual-parity or arbitrary-site compatibility result.
- [Static checks](idb-final-static-checks.json) passed Python and fixture JavaScript syntax, compiled adapter mirror equality, Chromium patch reverse-application and `git diff --check`.

The assertions cover schema creation and renaming, inline auto-increment keys, Unicode, unique and multiEntry indexes, handled constraint failures, generator rollback, transaction abort, range cursors, reverse/unique traversal, prefetch reset, Blob/File/typed-array/date values, index population ordering, chunked getAll, overlapping transaction ordering, version blocking and cooperative close, version rejection, upgrade rollback, cross-origin isolation, binary/array/date/infinite keys, quota rollback and persistent data.

The initial renderer failure was a key-injection error: an out-of-line-key value was returned with an injection key even though its key path was null. Injection is now limited to auto-increment stores with an inline path. An intermediate recovery fixture also reopened a restored version-2 test database at version 1. The fixture now removes its disposable database before recreating it, while retaining and checking its independent persistence database. Earlier evidence prefixes describe their own images and fixtures.

### Remaining requested services

The [final JavaScript service results](idb-final-service-capabilities-service-capabilities-results.json) show:

| Service | Observed result | Remaining implementation |
| --- | --- | --- |
| IndexedDB | Transaction write/read succeeds; 37 integration assertions pass. | Full conformance, browser-owned isolation, workers and quota management. |
| Dedicated worker | Worker startup error. | Host factory, actual thread/TLS execution, fetch context and lifecycle. `pthread_create` still returns `ENOTSUP`. |
| Media | Media error 4. | WebMediaPlayer, decoder integration and audio/video output. No playback was achieved. |
| WebGL | No context provider. | Context/device service and graphics backend. |
| WebGPU | No adapter. | Dawn/device service and graphics backend. |
| Renderer sandbox | Runtime reports `cpl=0`. | Unprivileged Blink/V8 execution, restricted mappings, validated IPC and fault/syscall handling. |

This change does not implement worker execution, media playback, GPU services or a security sandbox. Shared/service workers were not executed. The full default regression suite, Chromium browser tests, WPT, physical hardware and adversarial sandbox tests were not run. Human review and validation remain required before accepting these AI-assisted changes.

## Reproduction

Start the existing fixtures in separate terminals:

```sh
python3 tests/network-servers.py
python3 tests/websocket-server.py
```

Build and run from the project directory:

```sh
python3 build-blink-libraries.py --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
python3 tests/state-suite.py --https-ca build/network-https/ca.esl \
  --prefix idb-check indexeddb service-capabilities cache websocket storage network-secure tabs-site
python3 tests/indexeddb-persistence.py --https-ca build/network-https/ca.esl \
  --prefix idb-persistence-check
python3 tests/state-suite.py --cpus 1 --https-ca build/network-https/ca.esl \
  --prefix idb-single-check indexeddb service-capabilities
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

Use a fresh prefix. IndexedDB and service diagnostics use dedicated raw FAT disks. The generated fixture CA is supplied only to test guests. Tests navigate with native QEMU input events and execute normal page JavaScript; no DOM injection or host-browser storage substitutes for the guest implementation.
