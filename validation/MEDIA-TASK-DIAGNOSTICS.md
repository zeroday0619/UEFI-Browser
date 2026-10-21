# Scheduler task diagnostics

The live batching image completed YouTube controls but failed real-time
playback checks. Its existing pump-gap counters identify stalls without naming
the task responsible. The opt-in scheduler observer adds posting-source
attribution for tasks lasting at least 50 ms.

`TaskDiagnostics` registers with the thread's Chromium task scheduler after
creation and unregisters before shutdown. Browser and renderer instances are
independent. A fixed 32-entry stack retains nested debugger-loop timestamps;
deeper nested tasks are omitted without overwriting outer samples. Records
include elapsed wall time, queue wait where available, prior blocking/priority
state, nesting depth, process role and the task's posting file, line and
function. A queue-wait value of `-1` means no eligible timestamp was recorded.

Elapsed time includes nested loops, debugger pauses and host scheduling. A
posting location identifies the scheduled callback, not necessarily the
expensive function inside it. Logging and timestamp reads add overhead. This
diagnostic does not preempt JavaScript or provide independent media execution.

## Build and local checks

The option defaults to `0`, leaving the observer unregistered. Build the native
browser archive before linking the diagnostic image:

```sh
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/media-task" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/media-task" python3 validate-binary.py
```

Native compilation, EFI linking and binary validation passed. The EFI SHA-256
is `f501167ad88a75043097f2f70eb736443f2d4aa603e4dfcc845ee79b56b84bd9`.
`media-task-sources.json` records the source and image identities.

With the fixture server using `build/media-batch-https`, the command was:

```sh
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/media-task \
  --https-ca build/media-batch-https/ca.esl --prefix media-task \
  --require-cpl3 --require-no-renderer-faults devtools mse-batch
```

Both scenarios passed: DevTools in 87.39 seconds and queued-work MSE audio in
36.35 seconds. The renderer executed at CPL3 without unexpected faults.
The DevTools run recorded nested tasks at depth 1 during debugger use and
completed step/resume and teardown. These checks exercise registration,
normal and nested reporting, and removal; they are not a measurement of
diagnostic overhead or complete concurrency coverage.

## Live attribution and shutdown failure

`media-task-youtube-suite.json` records a 194.33-second diagnostic run with
90 seconds of initial observation, followed by Inspector entry and a typed
player-state query. The Console became ready in 16.69 seconds and returned
`YTSTATE 3 49.8935 2 null`. This run did not execute the full playback-control
probe and does not establish real-time playback.

`media-task-youtube-long-tasks.json` retains the largest of 451 reported tasks.
The longest observed task after Inspector entry lasted 11.142 seconds and was
posted from Mojo `Connector::PostDispatchNextMessageFromPipe`. Page-load
callbacks included 3.955 seconds from `DOMViewTransition::DidSkipTransition`
and 2.608 seconds from `HTMLParserScriptRunner::PendingScriptFinished`.
These identify scheduled execution paths. The specific expensive Inspector
command and the functions within those callbacks remain unmeasured.

The strict suite failed on a renderer NMI during shutdown, after the player
query and presentation summary. The prior shutdown implementation forced an
NMI after two seconds even while the renderer destroyed its browsing contexts.
The failed image and logs are retained. The subsequent bounded teardown change
and its verification are recorded in [RENDERER-TEARDOWN.md](RENDERER-TEARDOWN.md).

The diagnostic command was:

```sh
YOUTUBE_INSPECT=1 YOUTUBE_CONSOLE_TIMEOUT=120 \
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/media-task \
  --https-ca build/media-batch-https/ca.esl --prefix media-task-youtube \
  --require-cpl3 --require-no-renderer-faults youtube-observe
```

The explicit live scenario now creates a fresh profile disk and captures AC97
output through the same suite runner. It remains outside the default local
regression cases.

The follow-up `renderer-teardown` image passed the same live inspection and
exit check. Its acknowledged cleanup took 2.27 seconds without NMI, exceeding
the old shutdown deadline. This fixes the observed exit failure while leaving
long-task media starvation unresolved.

[Inspector command attribution](INSPECTOR-COST.md) separates protocol-command
phases and JIT gate costs, and records a controlled redundant-TLB-flush fix.
