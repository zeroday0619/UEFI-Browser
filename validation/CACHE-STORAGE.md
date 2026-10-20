# Cache Storage, Blob transport and service probes

The GPU availability statements below describe this report's recorded build. See [software GPU validation](SOFTWARE-GPU.md) for the subsequent ANGLE, Dawn and SwiftShader implementation.

This report records the Cache Storage image identified below. Cache Storage and Blob byte transport connect to Blink in that image. IndexedDB was added subsequently; see [IndexedDB validation](INDEXEDDB.md). Worker execution, media playback, GPU device services and a renderer sandbox remain unimplemented. The service probes record those failures explicitly; a completed probe is not a working implementation of the API.

## Implementation

- `src/blink/cache_storage.cc` implements named caches, open/has/delete/keys, match/matchAll, request keys and batch put/delete. Query matching handles methods, fragments, search parameters and Vary, including absent versus empty request headers. The backend queues operations per serialized Chromium storage key. Batch candidates replace committed entries only after a successful snapshot write; failed validation, body reads, quotas or persistence retain earlier in-memory entries.
- `src/blink/blob_service.cc` implements immutable byte blobs, composition, slices, clone references, byte-provider replies, stream registration and asynchronous Mojo data-pipe reads. Transfers use bounded chunks on the renderer sequence. Bodies are reconstructed as fresh Blob references on cache reads, so consuming one Response does not consume the cached body.
- The frame broker binds CacheStorage only for a current non-opaque, potentially trustworthy frame storage key. The process broker binds BlobRegistry. Native WebSocket and Web Storage paths remain connected.
- The Chromium patch binds BlobBytesProvider to the current renderer sequence in UEFI builds. Its upstream thread-pool binding never ran in this runtime and stalled large Blob reads.
- Document and subresource responses now set a non-null, empty HTTP status text. The firmware response does not expose a reason phrase. Leaving the Blink string null caused outgoing Cache Storage messages to fail Mojo validation during `addAll()`.
- Media-control resources are embedded from Chromium's generated 100-percent GRIT pack. Missing resource 43486 previously aborted the renderer while displaying a media-load failure.
- A missing dedicated-worker host factory now dispatches the worker error event asynchronously instead of dereferencing null. The WebGPU provider callback now completes with no provider, allowing `requestAdapter()` to resolve without an adapter. These changes provide failure handling, not worker execution or GPU support.

## Persistence and limits

A profile snapshot stores cache names, request/response metadata and body bytes under a SHA-256-derived storage-key filename. Two files alternate generations. Loading checks the format marker, storage identity, checksum, collection limits and Mojo metadata validity, then selects the highest valid generation. Writes flush the alternate file before committing the candidate in memory.

The snapshot format includes the pinned Chromium version `158.0.8110.0` because its metadata uses generated Mojo serialization. A Chromium schema change requires a format migration or new version marker. A version mismatch is rejected; existing data is not migrated automatically.

Limits are 32 storage keys, 32 named caches per key, 256 entries per cache, an 8 MiB snapshot per key, 64 MiB of accounted snapshots and 128 live cache objects including deleted caches retained by handles. Additional accounting bounds retained entry payloads. Individual blobs are limited to 8 MiB, live completed Blob payloads to 64 MiB and simultaneous pipe transfers to 64. These are service quotas, not a bound on all renderer heap allocations or transient serialization copies.

An open handle keeps a deleted cache usable for its lifetime, while reopening its name creates an independent cache. Deleted handles are no longer persisted. There is no eviction UI, StorageManager integration, service-worker support or complete browser quota service. The browser-internal `GetAllMatchedEntries` inspection method is not implemented.

Blob URLs, file-backed blobs and side data remain unsupported. Cache reads preserve opaque response filtering in the tested path, but complete COEP/CORP policy and Web Platform Test conformance are unverified. Snapshot recovery covers an invalid application payload checksum. It does not guarantee FAT metadata atomicity or recovery from physical power loss. Snapshot encoding and file writes run synchronously on the renderer; long JavaScript can delay these services. Descriptor checks and storage-key checks do not establish a security boundary against a compromised CPL0 renderer.

## Verification

Final EFI SHA-256: `4d755fbaa330531f462368b9f89f160c0f73685a98727e2a5434c0e7c7604482`.

The build uses Chromium 158.0.8110.0, musl 1.2.5 and Debian Clang 23.1.3. Execution used QEMU 11.1.2 with OVMF and KVM.

- The [six-scenario QEMU suite](cache-release-suite.json) completed: Cache Storage, service diagnostics, WebSocket, Web Storage, secure networking and live-site tabs. Cache Storage passed 35 JavaScript assertions. The diagnostics scenario records unsupported APIs and must not be interpreted as their functionality passing.
- [Three independent raw-disk boots](cache-release-persistence-results.json) each passed all 35 assertions. The persistent counter advanced 1, 2, 3. Before boot three, the test corrupted the latest snapshot at generation 46. The runtime loaded generation 45 and preserved the counter. Host-side FAT reads independently checked snapshot hashes.
- The [single-CPU suite](cache-single-cpu-suite.json) exercises Cache Storage and the same service diagnostics without an AP renderer.
- [Cache Storage](cache-release-cache-input-cache-checks.png), [service diagnostics](cache-release-service-capabilities-input-service-capabilities.png) and [zeroday0619.dev](cache-release-tabs-site-input-tabs-site-interactive.png) screenshots were inspected. The live-site scenario also exercises native tabs and settings. This does not establish arbitrary-site compatibility or visual parity.
- [Native compilation](cache-release-native-compilation.log), [EFI linking](cache-release-link.log) and [binary validation](cache-release-binary.log) passed. The binary validator reports no required unresolved symbols, imports or host syscall instructions. Existing compiler warnings remain.
- Python syntax checks, shell syntax checks, Chromium patch reverse-application checks and `git diff --check` passed. The full default suite, WPT, physical hardware and adversarial sandbox validation were not run.

The 35 cache assertions cover empty/composite/large blobs, Unicode and NUL bytes, 262,145-byte binary bodies, repeated body reads, response metadata, cache ordering, query options, Vary, replacement, delete, global/named match, invalid put rejection, network `addAll()`, failed `addAll()`, concurrent writes, deleted handles, recreated caches, cross-origin isolation, opaque responses, quota rollback and persistence.

### Service results for this historical image

The [actual JavaScript probe results](cache-release-service-capabilities-service-capabilities-results.json) are:

| Service | Final observed result | Missing implementation |
| --- | --- | --- |
| IndexedDB | `AbortError: The connection was closed.` | IDBFactory backend, transactions, indexes/cursors, persistence and lifecycle integration. |
| WebGL | No context provider. | Graphics context and device service. |
| WebGPU | No adapter; the promise completes. | Dawn/device service and hardware or software GPU backend. |
| Media | Media error 4; no renderer crash. | WebMediaPlayer, decoder pipeline and output devices. |
| Dedicated worker | Worker startup error; no renderer crash. | Worker host, native threads/TLS, fetch context and worker lifecycle. |
| Sandbox | Native runtime log reports renderer `cpl=0`. | Unprivileged Blink execution, restricted mappings, validated IPC and exception/syscall handling. |

Shared workers and service workers were not exercised by this probe and remain disconnected. Skia CPU painting is not WebGL/WebGPU support. Fixed CPL3 presentation workers do not sandbox Blink or V8. Human review and validation remain required before accepting these AI-assisted changes.

## Failures retained for diagnosis

- `cache-initial-*`: a large Blob stalled because its byte provider was queued on the unavailable thread pool.
- `cache-bytes-provider-*`: cache/network integration reached `addAll()`, then aborted on null `FetchAPIResponse.status_text`.
- `cache-regressions-service-capabilities-*`: a media-load error aborted on missing media-control resource 43486.
- `service-media-resources-*`: media failure returned normally, then dedicated-worker startup faulted before its result.
- `cache-persist-first-*`: the first two guest boots succeeded, but the host corruption helper assumed a contiguous file. The helper now resolves the checksum byte through the FAT chain. Both Cache Storage and Web Storage persistence tests use the corrected helper.

Earlier manifests identify their own EFI hashes and are not attributed to the final image.

## Reproduction

Start the local fixtures in separate terminals. The WebSocket fixture uses the certificate generated by the HTTP fixture:

```sh
python3 tests/network-servers.py
python3 tests/websocket-server.py
```

Build and verify from the project directory:

```sh
python3 build-blink-libraries.py --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
python3 tests/state-suite.py --https-ca build/network-https/ca.esl \
  --prefix cache-check cache service-capabilities websocket storage network-secure tabs-site
python3 tests/cache-persistence.py --https-ca build/network-https/ca.esl \
  --prefix cache-persistence-check
python3 tests/state-suite.py --cpus 1 --https-ca build/network-https/ca.esl \
  --prefix cache-single-check cache service-capabilities
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

Use a fresh prefix for each run. Cache tests create dedicated raw FAT images; they do not mount or modify a host profile. The generated CA is supplied to the test VM only. Runtime evidence, fixture pages, screenshots and per-image manifests are retained under `validation/`.
