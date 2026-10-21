# CJK input integration

## Korean engine and editor boundary

The Korean adapter uses libhangul revision `23d2cd9d10d516d523d60c562dbcc7ea08139a09` with keyboard `2` and syllable output. The earlier local composition algorithm has been removed. The three unmodified upstream character, input-context and keyboard compilation units are linked statically. External keyboard XML loading and localization are disabled; no Expat or gettext dependency is introduced. The Hanja dictionary module is not linked.

`src/blink/korean_input.cc` owns one libhangul context per editor, converts its UCS-4 commit/preedit output to UTF-16 and resets state on finish or cancellation. The address bar uses its own context and CJK font fallback. Blink updates run on its input task queue through `WebInputMethodController`; native text pointers do not cross the renderer boundary. Input protocol version 2 carries scalar input-mode and finish-action fields within the existing 56-byte record. Navigation clears pending composition state. Focus changes triggered by a commit handler do not receive the following syllable in a different editor.

F9 toggles `EN`/`KO`. Ctrl+Space is an alternative when the firmware reports the space character. Enter confirms pending composition, Escape cancels it and Backspace reverses its most recent composition step. Korean mode routes editing keys to the renderer instead of speculative keyboard scrolling.

## Verification

Commands:

```sh
python3 tests/korean-input.py
python3 build-blink-libraries.py --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
python3 tests/network-servers.py
python3 tests/state-suite.py --cpus 4 --workers 2 \
  --https-ca build/network-https/ca.esl --prefix libhangul --require-cpl3 ime renderer-scroll heavy-site
```

The sanitizer host test exercises all 11,172 modern Hangul syllables, compound vowels/finals, final resyllabification, deletion, reset and independent contexts. `libhangul-host.log` records the result. `libhangul-native.log` and `libhangul-link.log` record the firmware build. Upstream compilation emits a size-comparison warning; existing browser build warnings also remain.

The QEMU scenario injects ASCII key events through the guest keyboard. It checks `한글`, deletion from `관` to `과`, cancellation of an additional syllable, `값` in contenteditable and `일가` in a same-origin iframe. It asserts compositionstart/update/end and composing input events, captures the native address bar with `한글` preedit, replaces the address and checks Latin input after switching mode. It does not inject text through a debugging DOM API. All three QEMU scenarios passed with CPL3 renderer execution: `ime`, `renderer-scroll` and `heavy-site`. Final image identity and execution results are recorded in `libhangul-suite.json`. The native address screenshot `libhangul-ime-input-ime-korean-address.png` was visually inspected for Hangul glyphs and the preedit underline.

The follow-up run repeated `python3 tests/korean-input.py` successfully and ran the same `ime` scenario with `--workers 1 --prefix libhangul-recheck --require-cpl3`. It passed in 51.37 seconds with renderer CPL 3 and the same EFI image hash. `libhangul-recheck-suite.json` records the result. The address-bar screenshot was visually checked for `한글` and its composition underline.

This is not complete IME conformance. Physical firmware key handling, Hanja conversion, alternate Korean layouts, password-policy behavior, accessibility and all JavaScript focus/cancellation cases remain unverified. Long renderer tasks can still delay page text composition. The native address renderer provides glyph fallback but is not a complete bidirectional shaping editor.

### Verification after the libhangul integration request

`python3 tests/korean-input.py` passed again with AddressSanitizer and UndefinedBehaviorSanitizer. The upstream keyboard compilation retained its existing constant-range comparison warning. The QEMU command below passed in 51.36 seconds with renderer CPL 3:

```sh
python3 tests/state-suite.py --cpus 4 --workers 1 \
  --https-ca build/network-https/ca.esl \
  --prefix libhangul-request --require-cpl3 ime
```

The fixture server was started with `python3 tests/network-servers.py` and stopped after the test. `libhangul-request-host.log` and `libhangul-request-suite.json` retain the results. The address screenshot `libhangul-request-ime-input-ime-korean-address.png` was visually checked for `한글` and its composition underline. This run used the existing `build/blink` EFI image with SHA-256 `8149e4122f3062404dd7ef7cfcb62a31ceea9320b4d42ccfe1e5ae3db0a2917c`; it did not rebuild the firmware or validate unrelated pending changes.

## Japanese engine and editor integration

Mozc revision `0136c35782536cb43d710575c311cd4185982738` is pinned in `revisions.json`. `fetch-mozc.py` verifies its source and dictionary notices. The host `//session:session_handler_main` target built with Bazel 9.0.2. A real dictionary session converted `nihongo` to `にほんご`, displayed the `日本語` candidate and committed `日本語`; see `mozc-conversion.log`.

`prepare-mozc.py --bazel build/tools/bazel-9.0.2` builds host generators, extracts the production compilation graph, copies generated tables and regenerates Protobuf code using Chromium's host protoc. The firmware graph includes the full engine and embedded OSS dictionary, with directory-specific GN source sets to avoid object-name collisions. Quoted include directories prevent Mozc's `base/` headers from resolving to Chromium's unrelated headers. Explicit GN visibility patches allow Mozc to use Protobuf and the Abseil flag implementation.

Native compilation and EFI linkage passed. Initial QEMU execution exposed two runtime gaps: Unix home-directory discovery called the unavailable password database, and Abseil entropy initialization tried to open `/dev/urandom`. The adapter now selects the brokered `/profile/mozc` path. The Abseil patch selects musl `getentropy`, which uses the existing firmware RNG boundary. The corrected image passed dictionary conversion, candidate selection and commit in both CPL0 and CPL3. The probe reads the actual code-segment privilege level for each success record.

`text_composer.cc` shares Korean and Japanese editor state handling between the address bar and Blink. Japanese scalar cursor offsets are translated to UTF-16 offsets. `ime_popup.cc` draws candidate labels with CJK font fallback and retains candidate identifiers for mouse selection. The renderer draws candidates after page rasterization and suppresses document scroll-cache publication while the overlay is visible. Candidate pixels and objects do not cross the privilege boundary as native pointers.

Shift+F9 switches between Japanese and Latin modes. Space converts romaji-derived kana and opens candidates. Direction keys and candidate clicks select a conversion; Enter confirms it and Escape cancels it. `japanese-ime-scenario.py` exercises page fields, contenteditable, an iframe, the address bar and return to Latin input.

The adapter sets incognito mode and `NO_HISTORY` through `ConfigHandler` before engine creation. `CREATE_SESSION` does not consume those fields from `input.config`. The host adapter test checks the effective configuration as well as conversion, candidate focus and arrow movement, selection, deletion, cancellation and independent sessions. It passed against the real Mozc engine.

The QEMU `ime-japanese` scenario passed with a CPL3 renderer. It entered `日本語` through a candidate click, `感じ` in contenteditable and `日本語` in an iframe, checked composition events, selected `日本語` in the native address bar and returned to Latin input. Both the address candidate list and its selected value were visually inspected. An initial test incorrectly expected `漢字` as the first candidate for `kanji`; the pinned engine returns `感じ` in both host and firmware tests.

The probe image also passed the Korean `ime` and long-JavaScript `renderer-scroll` regressions. `mozc-verified-suite.json` records all three results and the image hash. Every case contains successful Mozc probe records for CPL0 and CPL3. Commands:

```sh
python3 tests/japanese-input.py --bazel build/tools/bazel-9.0.2
python3 build-blink-libraries.py --mozc --jobs 20
ENABLE_BLINK=1 ENABLE_MOZC_PROBE=1 BUILD_DIRECTORY="$PWD/build/mozc-probe" ./build-musl.sh
python3 tests/state-suite.py --cpus 4 --workers 2 \
  --https-ca build/network-https/ca.esl --build-directory build/mozc-probe \
  --prefix mozc-verified --require-cpl3 --require-mozc-probe \
  ime-japanese ime renderer-scroll
```

The default `JOBS=20 ./build-blink.sh` build now includes Mozc and omits the diagnostic conversion probe. Its static binary validation passed. The same three QEMU scenarios passed again with CPL3 execution using `--prefix cjk-release --require-cpl3`; `cjk-release-suite.json` records the final image hash and results. The final address-bar screenshot was visually inspected. `cjk-release-sources.json` records the input-source hashes and confirms that they match the files used by the native build.

Candidate overlays are excluded from asynchronous document scroll caching while visible. Page composition still waits for the renderer task queue. Native candidate painting uses glyph fallback rather than a complete shaping editor. Arbitrary multi-segment conversions, all focus/selection/script interactions, physical Japanese keyboards, accessibility, user dictionaries and persistent Mozc preferences remain unverified. The current runtime logs unsupported filesystem metadata/history operations; these do not prevent the tested embedded-dictionary conversions. The tested configuration disables input history learning.

Mozc's engine, OSS dictionary and usage dictionary notices are retained separately. Their terms do not all reduce to the engine's BSD license.
