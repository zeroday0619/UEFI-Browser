# Bounded renderer shutdown

The `media-task-youtube` diagnostic completed its live player query but failed
the no-fault gate during browser exit. The log recorded an NMI after the
presentation summary and browsing-context destruction output. The previous
`StopRenderer` requested termination after two seconds without distinguishing
an unresponsive renderer from one completing shutdown.

The browser now requests shutdown before releasing its native tab objects.
The renderer checks the request before processing queued commands or publishing
frames, acknowledges it and leaves the event loop. It disables renderer polling
before destroying browsing contexts so nested cleanup cannot publish a partially
destroyed tab.

The BSP initially allows two seconds for acknowledgement. Once acknowledged,
the deadline becomes fifteen seconds from the start of the join operation.
Repeated or forged shared-channel changes cannot renew that deadline. The
shared acknowledgement grants time only; it grants no memory or service access.
At expiry the existing NMI termination and quiescent reclamation path remains
available. Firmware-service and join processing can add latency beyond the
deadline; this is not a hard real-time guarantee.

Explicit process-monitor termination retains its existing behavior. The browser
releases the shared channel only after the AP joins. Logs record whether shutdown
was acknowledged or forced and its elapsed time. A forced shutdown is not
silently converted into a successful no-fault result.

## Build and regression checks

The diagnostic image includes the media batching implementation and opt-in
scheduler task observer. Its EFI SHA-256 is
`204fc430b19d37418200439798db222147aa10ef7999c8e2b4690506c6b5b742`.
Native compilation, EFI linking and static binary validation passed.
`renderer-teardown-sources.json` records source and image identities.

```sh
ninja -C ../../work/chromium-build/chromium/out/uefi-blink-native -j4 \
  obj/third_party/blink/uefi/libbrowser.a
BLINK_TASK_DIAGNOSTICS=1 ENABLE_BLINK=1 \
  BUILD_DIRECTORY="$PWD/build/renderer-teardown" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/renderer-teardown" python3 validate-binary.py
```

With `tests/network-servers.py --directory build/media-batch-https` running:

```sh
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/renderer-teardown \
  --https-ca build/media-batch-https/ca.esl --prefix renderer-teardown \
  --require-cpl3 --require-no-renderer-faults devtools mse-batch
```

Both scenarios passed with CPL3 execution and no unexpected renderer fault:
DevTools in 87.37 seconds and queued-work MSE audio in 36.37 seconds. The latter
acknowledged shutdown and joined without forcing termination in 98,371 us.

The infinite-JavaScript shutdown case passed separately in 28.63 seconds.
The renderer could not acknowledge the stop request. The browser forced NMI
termination and joined in 2,103,338 us. The test requires the expected vector 2,
the unacknowledged/forced log, a 2-to-10-second shutdown interval and channel
release after join. Its intentional NMI is kept outside the no-fault suite:

```sh
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/renderer-teardown \
  --https-ca build/media-batch-https/ca.esl \
  --prefix renderer-teardown-infinite --require-cpl3 renderer-shutdown
```

See `renderer-teardown-infinite-suite.json`. Repeated or forged acknowledgement
states have been reviewed in source but have not been adversarially injected
in QEMU. Physical-machine shutdown and firmware-specific NMI behavior remain
unverified.

## Live YouTube exit

`renderer-teardown-youtube-suite.json` records the corrected live scenario,
which passed in 197.97 seconds at CPL3 without a renderer fault. The Console
became ready in 18.72 seconds and a typed query returned
`YTSTATE 1 63.9180625 3 null`. Browser exit was acknowledged and completed
without forced termination in 2,271,116 us. That measured cleanup exceeds the
previous two-second deadline. The earlier failing image remains available in
`build/media-task`; it was not overwritten.
The 90-second video screenshot and the Inspector command screenshot were
visually inspected. Missing sidebar thumbnails and server/resource errors
remain visible; full-page fidelity is not established.

```sh
YOUTUBE_INSPECT=1 YOUTUBE_CONSOLE_TIMEOUT=120 \
QEMU_TEST_REQUIRE_WATCHDOG_OFF=1 python3 tests/state-suite.py \
  --cpus 4 --workers 1 --build-directory build/renderer-teardown \
  --https-ca build/media-batch-https/ca.esl \
  --prefix renderer-teardown-youtube --require-cpl3 \
  --require-no-renderer-faults youtube-observe
```

This verifies the observed shutdown regression. It does not rerun or supersede
the full control-interval playback measurements in `MEDIA-BATCHING.md`, which
still failed real-time progression. The default `build/blink` image remains
unchanged. Launch this isolated candidate with:

```sh
BUILD_DIRECTORY="$PWD/build/renderer-teardown" QEMU_MEMORY=4096 ./run-qemu.sh
```
