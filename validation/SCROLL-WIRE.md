# CPL3 asynchronous scroll transport

The default browser renderer publishes compressed pixel tiles and value metadata through a bounded mailbox. The BSP copies a publication into private memory, validates the complete message, and installs private scroll objects. Blink and V8 remain at CPL3. The BSP never follows a renderer `ScrollScene*`, invokes a supplied callback, or deserializes a received Skia picture or font program.

## Ownership and rendering

`remote_frame.cc` allocates the mailbox with the application channel before launching the AP. Its address and capacity are private BSP state. A publication supplies a byte length, not a buffer address. The shared `pixels` and legacy `scroll_scene` pointer fields are ignored by the CPL3 consumer. One producer publication owns the scroll mailbox until the BSP acknowledges it. Closing a tab releases an outstanding mailbox owner. Recovery clears the channel after AP quiescence.

The BSP reads the length once, checks its capacity and heap headroom, and copies exactly that many bytes. All parsing, graph traversal and rendering use the private copy and consumer-owned objects. A concurrent renderer modification can produce a rejected message or another valid visual scene; it cannot supply native object references. Invalid or oversized messages clear the asynchronous cache and retain normal renderer dispatch.

The renderer rasterizes at most 512×256 pixels at once. It reuses the original captured image and typeface references within its own address space. Re-deserializing typefaces on every publication created duplicate glyph-cache entries and exhausted the 128 MiB C heap during development; retaining the original references fixes that growth. These references are excluded from the wire format.

Chromium's existing zlib compresses each tile. The BSP validates every compressed stream with exact input consumption and output length. It retains compressed bytes and expands only visible tiles into the existing 32 MiB / 1,024-entry raster cache. Eviction of visible pixels does not discard recorded offscreen content. Hidden tabs release expanded raster tiles. Encoding releases the acknowledged temporary buffer before allocating its replacement. Tile-cache growth evicts existing tiles when tracked heap headroom is low. Source capture and encoding run on the renderer. Ordinary picture serialization yields between records. The current source also encodes wire pixels in cooperative tile tasks before viewport publication; see `SCROLL-WIRE-SLICES.md`. Individual Skia/zlib calls, layout and paint remain synchronous. See `SCROLL-ENCODING.md` and `SHARED-PAINT.md`.

Boundary wheel input is consumed even when no ancestor can move. Previously, those wheel events entered Blink's deferred relative-input queue and could run after newer absolute scroll commands, changing the final offset after JavaScript resumed. The scroll-state reconciliation regression covers boundary consumption. A later affine fixture exposed overtaking of an earlier blocking wheel event by a later absolute scroll update. The current source queues pointer, keyboard, scroll and scrollbar-state operations together; see `AFFINE-SCROLL.md`.

The iframe regression exposed a separate null input-manager dereference in `WebFrameWidgetImpl::SetMouseCapture`. The fault stack passed through `EventHandler::CaptureMouseEventsToWidget`, `WidgetInputHandlerManager::GetWidgetInputHandlerHost` and `pthread_mutex_trylock`. The non-composited embedder now guards mouse capture, mouse-lock and selection-autoscroll host notifications when the manager is absent. Local selection-autoscroll cleanup still runs. This fixes iframe pointer delivery; it does not add a pointer-lock host implementation.

## Version 4 source format and limits

Integers and IEEE-754 floats use explicit little-endian 32-bit words. Element IDs use two words. Booleans accept only 0 or 1. There is no native structure layout, padding, pointer, vtable or function address in the message.

The header contains magic `0x5343524c`, version `4`, and a reflow-scene count. The base scene is followed by flat reflow scenes. Each scene contains wheel flags and counted transform, clip, effect, wheel-region, scrollbar, gutter, query and picture records. Each picture then contains its appearance count and raster references. A zero raster reference introduces bounds and compressed tiles; a nonzero reference selects a previously decoded raster. Tile coordinates and dimensions derive from the validated bounds and fixed tile grid. A tile contains a byte length followed by one zlib stream. Trailing bytes are rejected.

Version 3 added `scale_x`, `skew_x`, `skew_y` and `scale_y` after the transform's
translation fields. The matrix includes the CSS transform origin. Version 4 adds
effect-local transforms and counted value records for supported CSS filters.
Both sides must use version 4; earlier messages are rejected by this decoder.
The default `build/blink` image remains on its previously tested source until
explicitly rebuilt or replaced. The affine candidate and evidence are described
in `AFFINE-SCROLL.md`. The filter candidate is described in `SCROLL-FILTERS.md`.

Each affine coefficient and each composed ancestor matrix must be finite and
bounded by 1,000,000 in absolute value. The compositor also checks the matrix and
its inverse after applying dynamic scroll/sticky offsets. Noninvertible or
out-of-budget transforms cannot participate in drawing or hit testing. Singular
CSS transforms are valid messages and produce no painted or hit-testable area.
A synthetic scrollbar-thumb transform must be a direct translation child of
its scrollbar transform, without scroll or sticky metadata.

| Limit or invariant | Enforcement |
| --- | --- |
| Shared message | 16 MiB; fixed BSP address; private copy before parsing |
| Total expanded pixels | 256 MiB per message as a decoding-work limit; not a resident bitmap allocation |
| Tile allocation | At most 512×256×4 bytes, plus bounded compression workspace |
| Unique rasters / tiles | At most 4,096 each across the message |
| Metadata / appearance references | Shared budget of 32,768 records |
| Scene graph | At most 2,048 pictures and 4,096 transforms/clips/regions per scene; 128 effects, scrollbars, queries, gutters or layouts per corresponding vector |
| Filters | At most 16 operations per effect, 32 per ancestor chain and 1,024 per scene; explicit kind, finite scalar/color/matrix values, bounded blur and shadow offsets |
| Filter layers | At most 512 pixels accumulated device-space outset and 64 MiB estimated concurrent layer storage, with tracked-heap headroom checked at installation |
| Reflows | At most 32 flat variants; no recursive object graphs |
| Raster extent | Width ≤ 8,192; height ≤ 32,768; nonnegative dimensions |
| Coordinates | Finite floats and bounded integers within ±1,000,000; sorted rectangles; valid radii |
| Parent chains | Parent precedes child; depth ≤ 64; every cross-vector index checked |
| Scroll state | Ordered minimum/current/maximum, nonnegative viewport, validated appearance/layout counts and enum ranges |
| Heap headroom | Checked before encoding growth, publication copy and decoding against the current runtime heap budget |

The initial capture has its own eligibility and resource limits. Scenes that exceed either capture or transport limits use renderer dispatch. The limits do not guarantee a worst-case UI latency against a renderer deliberately generating expensive valid messages.

## Implementation map

- `src/blink/scroll_wire.h`, `src/blink/scroll_wire.cc`: value codec, graph validation, tile compression/decompression, budgets and malformed-input self-test.
- `src/blink/scroll_snapshot.h`: process-local raster metadata and renderer-local source references.
- `src/blink/scroll_scene.cc`: original-resource retention, consumer-owned compressed tiles and visible-tile decoding.
- `src/blink/remote_frame.cc`: fixed mailbox, copy-before-validation, publication lifetime and test-only hostile producer helper.
- `src/process/cpl3_probe.h`: CPL3 test entry; retained only in the boundary-test build.
- `tests/input-scenario.py`, `tests/state-suite.py`: hostile publication scenario and `--require-scroll-wire` assertions.
- `config/blink-navigation.gn`: codec compilation and an explicit dependency on Chromium's already included zlib target.
- `patches/blink-build.patch`: persistent non-composited input-manager guards.

## Verification method

The startup self-test round-trips a raster and a blur effect. It rejects invalid filter kinds, counts, NaN/infinite values, negative/excessive blur, shadow offsets, tile modes, color alpha, matrix values and an overlong filter chain in an otherwise valid message. It also rejects every truncated prefix, invalid magic/version/count/boolean, NaN coordinates, cyclic or invalid parents, invalid raster references, oversized extents/lengths, corrupted compressed bytes and trailing data. The QEMU suite requires both this marker and a validated CPL3 wire publication.

The hostile-producer test redirects a user-mode AP instruction to a test-only entry. The entry aligns its own user stack and publishes five invalid messages: an oversized length, truncation, bad version, NaN coordinate and damaged compressed data. It also replaces the shared native-pointer fields with forbidden addresses. The BSP must reject all five, accept a following valid cache, scroll while the producer remains in a busy loop, and recover interactive page input after NMI termination. This test does not execute the producer at CPL0.

The separate sandbox scenario checks supervisor reads and page-table writes while a Blink page is open. Long-JavaScript tests require pixel changes before the end-of-task console marker and compare the composed page with the renderer's reconciled frame. Reconciliation counts channel differences greater than 2; a reported zero means equality within that tolerance. Screenshot timings include QMP capture and host scheduling; they are not physical display latency or worst-case bounds.

## Remaining boundaries

DOM handlers, cancelable wheel/keyboard paths, text editing, uncaptured content and unsupported paint properties still wait for the renderer. Tabs still share one renderer. V8's renderer-owned JIT pages may be RWX. The zlib decoder and BSP compositor remain trusted code. This change does not establish Chromium-equivalent sandbox coverage or replace an independent security audit.

## Reproduction commands

The fixture server must remain running throughout the suites so its generated HTTPS certificate does not change.

```sh
python3 tests/network-servers.py
```

In another shell, from the project directory:

```sh
python3 build-blink-libraries.py --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
ENABLE_BLINK=1 ENABLE_CPL3_PROBE=1 BUILD_DIRECTORY="$PWD/build/cpl3-probe" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py

python3 tests/state-suite.py --workers 2 --require-scroll-wire \
  --https-ca build/network-https/ca.esl --prefix scroll-wire-verified \
  scroll-controls scroll-state scroll-input renderer-scroll scroll-tree tabs-site heavy-site
python3 tests/state-suite.py --workers 2 --require-scroll-wire \
  --https-ca build/network-https/ca.esl --prefix scroll-wire-layouts \
  scroll-style scroll-layout scroll-gutter-container scroll-gutter-iframe scroll-stuck scroll-state-iframe
python3 tests/state-suite.py --workers 1 --build-directory build/cpl3-probe \
  --require-cpl3-probe --require-scroll-wire --https-ca build/network-https/ca.esl \
  --prefix scroll-wire-verified-isolation scroll-wire-boundary sandbox

# Focused reruns after the iframe input-manager fix.
python3 tests/state-suite.py --workers 2 --require-scroll-wire \
  --https-ca build/network-https/ca.esl --prefix scroll-wire-complete \
  scroll-state-iframe renderer-scroll scroll-input scroll-tree heavy-site tabs-site
python3 tests/state-suite.py --workers 1 --build-directory build/cpl3-probe \
  --require-cpl3-probe --require-scroll-wire --https-ca build/network-https/ca.esl \
  --prefix scroll-wire-complete-isolation scroll-wire-boundary sandbox

python3 -m py_compile tests/input-scenario.py tests/state-suite.py
git diff --check -- . ':!validation/*.log'
```

## Verified artifacts

Tests use QEMU 11.1.2 (Debian `1:11.1.2+ds-3`), OVMF `2026.08+ds-2`, KVM, q35, 4 vCPUs and 4 GiB RAM. Clang is Debian 23.1.3. QMP screenshots capture the guest GOP output; no host browser renders the tested pages.

- Normal EFI: `build/blink/esp/EFI/BOOT/BOOTX64.EFI`, SHA-256 `7c9d190d94ddd51ff375b7bcb52fac0758d615a92bbb995f6f363fad99c3661a`.
- Probe EFI: `build/cpl3-probe/esp/EFI/BOOT/BOOTX64.EFI`, SHA-256 `315f932fbb60413844ce04d39360a18fac0b8978ecafd361a490c1c6a02d5199`.
- Native build and both EFI links passed: `scroll-wire-native.log`, `scroll-wire-link.log`, `scroll-wire-probe-link.log`.
- Static binary validation passed: `scroll-wire-binary.log`. Test-only producer entry symbols are absent from the normal payload.
- Python syntax, diff whitespace, new codec whitespace and adapter-source synchronization checks passed.

The normal image executes all logged Blink/V8 event loops at CPL3. The probe image additionally passes all 11 startup privilege-boundary cases. Its live-page test rejects supervisor reads with page-fault error 5 and page-table writes with error 7, then reloads the page and accepts a click after each restart.

| Suite artifact | Result and scope |
| --- | --- |
| [Final normal-image rerun](scroll-wire-complete-suite.json) | 6/6 pass: iframe scroll-state, root scrolling, nested/sticky scrolling, keyboard/wheel/drag input, real-site interaction and real-site tabs. Every scenario verifies CPL3, the codec self-test and a wire publication. |
| [Final probe-image rerun](scroll-wire-complete-isolation-suite.json) | 2/2 pass: hostile publication rejection/recovery and live supervisor-memory/page-table protection. |
| [Hostile producer measurements](scroll-wire-complete-isolation-scroll-wire-boundary-scroll-wire-boundary-results.json) | Producer remains at CPL3; all 5 invalid publications rejected; forbidden native pointers ignored; valid cache accepted afterward; scrolling continues during the producer loop; click works after renderer restart. |
| [Earlier normal-image regression](scroll-wire-verified-suite.json) | 7/7 pass before the final input-manager guards, including horizontal/native/custom controls and top-level scroll-state queries. |
| [Earlier layout regression](scroll-wire-layouts-suite.json) | 5/6 pass: hover/pressed styles, scrollbar layout changes, container-query gutters, iframe gutters and stuck queries. The iframe scroll-state click fault was diagnosed, fixed and passed in the final rerun above. |

| Controlled workload | JavaScript task | First observed pixel change | Reconciliation |
| --- | ---: | ---: | --- |
| Root wheel scrolling, fixed controls, offscreen marker | 6 s | 19.83 ms | 0.0 differing-channel fraction |
| Nested/root scrolling with sticky elements | 12 s | 22.95 ms | 0.0 differing-channel fraction |
| Root keyboard scrolling | 12 s | 19.23 ms | 0.0 differing-channel fraction |
| Horizontal keyboard scrolling | 20 s | 41.98 ms | Full control scenario passes |
| Scroll-state top/middle/bottom layouts | 25 s | Pixel assertions during the task | 0.0 differing-channel fraction |
| Scroll-state layouts inside an iframe | 25 s | Pixel assertions with no renderer publication during input | 0.0 differing-channel fraction |

The nested test moves the root by 5,120 pixels and a nested area by 4,400 pixels. The input tests preserve cancelable wheel/keyboard behavior, editable and select controls, passive wheel notifications and native scrollbar dragging. The control test covers horizontal and vertical custom/native scrollbars, arrows, track paging, held-button repeat and negative RTL offsets.

The real-site scenarios verify `zeroday0619.dev` loading, controls and tab navigation. Its captured paint scene remains ineligible (`eligibility=10`), so these scenarios do **not** demonstrate asynchronous scrolling on that site. Long-task latency measurements above use the controlled fixture pages. Unsupported scenes retain renderer-dispatched scrolling.

The horizontal-keyboard and top-level scroll-state measurements come from the preceding normal image (`d5ea87b5995f63a4e89388509c257fa9713dfc2a1bf00377dde066ae0ee4ed95`). The root-wheel, nested, root-keyboard and iframe measurements come from the final image. The final normal and probe suites ran to completion with exit status 0. The normal image did not rerun every earlier layout/control scenario after the three input-manager guards.

Development runs named `scroll-wire-first`, `scroll-wire-second`, `scroll-wire-tiles`, `scroll-wire-bounds`, `scroll-wire-resources`, `scroll-wire-release` and `scroll-wire-final` retain intermediate failures and their original image hashes. The `scroll-wire-iframe-debug`, `scroll-wire-iframe-trace` and `scroll-wire-iframe-lock` runs contain diagnostic failures; GDB mutex breakpoints also distorted timing. They are not passing behavior evidence.
