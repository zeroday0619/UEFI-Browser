# V8 source positions before Inspector activation

## Change and scope

`build-musl.sh` accepts `BLINK_SOURCE_POSITIONS=lazy|eager`. The default remains
`lazy`. The eager mode appends `--no-enable-lazy-source-positions` to the selected
JavaScript mode. Invalid values are rejected. No new dependency is introduced.

In the pinned V8 tree, `Debug::UpdateState` calls
`Isolate::CollectSourcePositionsForAllBytecodeArrays` on debugger activation.
`SharedFunctionInfo::CanCollectSourcePosition` gates deferred collection on
`enable_lazy_source_positions`. Eager generation moves that work into normal
compilation. It preserves source positions and debugger features instead of
skipping the debugger activation protocol.

## Build and validation

The candidate EFI is `build/eager-source-positions/esp/EFI/BOOT/BOOTX64.EFI`,
SHA-256 `91fa42ad0d631ba4c0bcb46a640035c3674f7192cbf44611abf487c754cc7785`.
It links the existing native libraries used by the preceding record-reuse
candidate. The generated V8 flag string is retained in `browser-url.c` and the
boot log. Build, EFI binary validation, shell syntax and the generated Python
configuration syntax passed. Direct generation checks accepted both supported
modes and rejected an invalid mode.

```sh
BLINK_SOURCE_POSITIONS=eager BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/eager-source-positions" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/eager-source-positions" python3 validate-binary.py
YOUTUBE_LONG=1 YOUTUBE_INSPECT=1 YOUTUBE_CONSOLE_TIMEOUT=120 \
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/eager-source-positions \
  --https-ca build/inspector-timing-https/ca.esl --prefix eager-source-positions \
  --require-cpl3 --require-no-renderer-faults devtools youtube-observe
```

The local fixture server was already running with
`tests/network-servers.py --directory build/inspector-timing-https`.
The two QEMU cases passed at CPL3 without unexpected renderer faults: DevTools
in 88.06 seconds and live YouTube observation/inspection in 300.32 seconds.
The former exercises Console DOM/CSS mutation, debugger pause/step/resume,
inspector reuse and target detachment. The latter returns a playing video at
193.492 seconds with readyState 4 and no media error. It does not run the full
playback-rate or control gates on this candidate.

## Live comparison

Both images observed the live page for 180 seconds before opening Inspector.
The preceding lazy image is
`2e30b60b7318f443c9a474d7a35312877d6eb9715d8fbdf36d72c915e51c130b`.
`eager-source-positions-comparison.json` retains exact values and log prefixes.

| Measurement | Lazy | Eager |
| --- | ---: | ---: |
| Debugger.enable command | 18.423 s | 0.157 s |
| V8 enable phase | 18.291 s | 0.025 s |
| Compiled-script collection | 0.120 s | 0.120 s |
| Reported scripts | 305 | 303 |
| Console readiness after Console selection | 20.637 s | 24.292 s |

The command timing identifies a reduced synchronous debugger activation stall.
It does not demonstrate faster complete UI entry: Console readiness did not
improve. Each mode has one live sample; network content, script populations,
GC state and host scheduling were not identical. Startup CPU cost and retained
source-position memory require controlled comparison before changing defaults.
The default `build/blink` image remains unchanged.

## Remaining playback work

The strengthened visible-page measurement on the lazy image still fails at
0.847 media seconds per wall second. It excludes tab transitions during the
interval, so Inspector tab-switch overhead alone cannot explain the failure.
See `YOUTUBE.md` and `youtube-visible-measurement-sources.json`.
This change does not provide independent media execution during long JavaScript
or garbage collection, or establish sustained real-time audiovisual playback.
All nine implementation objectives remain active.
