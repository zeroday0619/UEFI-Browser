# Scroll-state queries, scrollbar topology and cookies

## Implementation

`CaptureScrollbarReflows` captures bounded `scroll-state(scrollable)` and `scroll-state(stuck)` variants using Blink's query evaluator and layout engine. The BSP selects a complete scene from the current scroll ranges or Chromium sticky constraints while JavaScript occupies the renderer. Scrollable states include both physical axes. Stuck states include the physical edges. Speculative overrides are confined to the capture pass and are cleared before returning to the live document.

Scroll-area IDs now derive from DOM node identity, so layout-object replacement does not disconnect queued input. The cache records the controlling gutter dimensions separately from the scene's scrollbar list. Dependent scrollbars may appear or disappear without invalidating otherwise supported scenes. Surviving areas retain their absolute offsets; areas removed from the scene reset their retained offset. Unsupported capture or query state spaces fall back to renderer handling.

On renderer reconciliation, query-dependent layout is resolved before applying the queued scroll coordinate. Sticky-query descendants receive an additional bounded layout/snapshot update so a changed scroll range does not clamp against the old document height. This synchronization runs only when the published scene contains query metadata. Pages without such queries keep the ordinary scroll-offset path.

The cache is limited to 64 combined gutter/query states and 16 MiB of serialized data and geometry. A single scrollable query contributes 16 possible axis-state combinations; a stuck query contributes nine. Unsupported combinations, exhausted budgets, `snapped`, `scrolled`, mixed stuck/scrollable dependencies on one container, anchored queries, remote frames and removal of a controlling scrollbar retain renderer handling. This is a cache of the last published DOM, not concurrent DOM execution or a complete Chromium compositor.

First-party document and HTTP cookies use Chromium's canonical cookie parser and access checks in a bounded session jar. See [Chromium port status and cookie policy](CHROMIUM-PORT-STATUS.md) for supported paths and remaining browser services.

## Reproduced defects

- A missing sticky scroll ancestor was passed to a nonzero-only compositor ID constructor. The initial QEMU run failed its DCHECK. Empty IDs now remain empty.
- Queued scroll offsets were clamped against the document height before scroll-state styles increased it. The initial scrollable-query test measured a 28.6885% component mismatch after renderer reconciliation. Query-dependent layout now precedes final coordinate clamping.
- Restoring a changed speculative offset used `kClamping` with `kNone`. Chromium requires `kStationaryScroll`; the QEMU DCHECK exposed this previously dormant path.

## Validation method

The query fixtures execute an uninterrupted 25-second JavaScript loop. QMP sends actual wheel, pointer and button input during that loop. Screenshots verify query colors, text wrapping, scrollbar presence and sticky descendant height. The tests require no renderer scene publication during input and compare the busy framebuffer with the live renderer after it resumes. Pixel components may differ by two levels; the comparison fails if more than 2% exceed that tolerance. These checks do not measure optical latency or guarantee a frame rate.

The topology fixture counts pixels belonging to a dependent scrollbar as hover and pressed thickness changes create and remove it. The replacement fixture changes an existing scroll area's layout object. A filtered variant verifies renderer fallback. Child-frame fixtures verify the same identity and layout paths across local frame boundaries.

The cookie fixture verifies document access, HttpOnly read/write protection, HTTP transmission, domain/path checks, expiry and fetch credentials. A separate HTTPS fixture verifies Secure transmission, plaintext exclusion, HttpOnly and cookie prefixes. Its generated CA is passed only to the test VM; the normal browser trust store is unchanged.

## Results

All 20 QEMU cases passed on the same final EFI image. The ten query/gutter framebuffer comparisons measured a 0.0 differing-component fraction after renderer reconciliation. The blocked-renderer query tests recorded zero renderer scene publications during input. The replacement fixture retained its nested scroll offset of 40 pixels through speculative capture. The existing scroll-input regression measured a fraction of 0.0000018328445747800586 (approximately 0.000183%); it passed its comparison threshold. All other recorded reconciliation comparisons were zero.

| Group | Cases | Result |
| --- | ---: | --- |
| Scrollable, stuck and iframe query states | 3 | Passed |
| Query/gutter interaction, scrollbar creation/removal, layout-object replacement and filtered fallback | 4 | Passed |
| Parent/nested iframe, root and horizontal gutter regressions | 4 | Passed |
| Scrollbar layout, controls, style, keyboard input and nested scroll tree | 5 | Passed |
| HTTP and HTTPS cookie integration | 2 | Passed, with 15 HTTP checks and 8 HTTPS checks |
| Live `zeroday0619.dev` loading and tab switching | 1 | Passed |
| Frame origin, sandbox, inherited CSP and teardown policy | 1 | Passed |

The Blink dependency build, EFI link, binary validation, Python/JavaScript syntax checks, reverse patch check and whitespace check passed. The binary validator found no required unresolved symbols or syscall instructions.

Environment: Chromium 158.0.8110.0 (`f12c350cb1e1e7ea05c11d6045b0a04158ec9e28`), Debian clang 23.1.3, QEMU 11.1.2, OVMF 2026.08+ds-2, x86-64 KVM and 1280×800 GOP.

Final EFI SHA256: `7a37fca93406d7debe7f3f10fbf0a1fffc9c69b495b93c76ad94fff64334e36c`.

Evidence: [suite results](state-release-suite.json), [source and image hashes](state-release-hashes.json), [binary validation](state-binary.log), [static checks](state-static-checks.json), [scrollable-query measurements](state-release-scroll-state-scroll-state-results.json), [stuck-query measurements](state-release-scroll-stuck-scroll-state-results.json) and [iframe-query measurements](state-release-scroll-state-iframe-scroll-state-results.json).

Framebuffer captures: [dependent scrollbar created while JavaScript runs](state-release-scroll-gutter-topology-input-gutter-hover-busy.png), [sticky descendant expanded while JavaScript runs](state-release-scroll-stuck-input-state-expanded-busy.png) and [scrollable state before renderer recovery](state-release-scroll-state-input-state-final-busy.png).

## Changed files

- `src/blink/scroll_scene.cc`, `scroll_scene.h` and `scroll_snapshot.h`: stable identities, query/gutter scene selection, topology changes and renderer synchronization.
- `patches/blink-build.patch`: UEFI query snapshot overrides and the synchronous document-cookie bridge.
- `src/blink/cookies.cc`, `cookies.h`, `navigation.cc`, `navigation.h`, `resource_loader.cc`, `browser_frame.cc`, `src/network/resource.h` and `config/blink-navigation.gn`: cookie storage, header integration and build wiring.
- `tests/scroll-state.py`, gutter fixtures, cookie fixtures, `input-scenario.py`, `redirect-server.py` and `state-suite.py`: QMP behavior tests, HTTP(S) endpoints and isolated suite execution.
- `README.md` and validation reports: current support boundaries, evidence and reproduction commands.

## Reproduction

Build and validate the image:

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

Run the three loopback fixture servers in separate terminals:

```sh
python3 tests/redirect-server.py
```

```sh
python3 - <<'PYTHON'
import runpy
from http.server import ThreadingHTTPServer
handler = runpy.run_path('tests/redirect-server.py')['Handler']
ThreadingHTTPServer(('127.0.0.1', 8766), handler).serve_forever()
PYTHON
```

```sh
python3 tests/https-fixture.py --directory build/state-https --port 8767
```

Run the suite:

```sh
python3 tests/state-suite.py --https-ca build/state-https/ca.esl
```

The suite uses two QEMU/KVM workers with separate FAT directories, OVMF variable files and serial/QMP endpoints. Each VM has four vCPUs and 4 GiB RAM. Logs, screenshots, per-case measurements and the image hash are written under `validation/state-release-*`. Use `--workers 1` for sequential execution.

Full Chromium WPT/browser suites were not run. Persistent cookies, partitioned/third-party cookies and WebSocket transport remain unimplemented. The broader remaining functionality is listed in [Chromium port status](CHROMIUM-PORT-STATUS.md).
