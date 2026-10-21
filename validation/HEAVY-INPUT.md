# Input under JavaScript and rendering load

Validated on 2026-10-18 with QEMU 11.1.2, OVMF 2026.08, Clang/LLD 23.1.3, 4 GiB guest RAM and a 1280 × 800 framebuffer. The browser remains a single-process UEFI application. No production dependency was added. The final image and driver hashes are in `heavy-result.json` (local artifact).

## Findings and changes

The original VirtioInputDxe polled every 50 ms and collapsed all received pointer events into one state. Under CPU load, a 30 ms press/release could disappear before the browser read it. The original image reproduced this failure in `heavy-fixture-before-kvm.log`. The driver now polls every 5 ms and queues complete SYN_REPORT records. A full queue leaves device reports unread rather than overwriting button edges. The host test compiles the actual patched handler and GetState bodies with protocol stubs; 128 queued clicks, wraparound, relative movement and wheel saturation pass under ASan/UBSan. QEMU confirms that five 30 ms clicks reach the page during repeated CPU/layout work.

V8 now uses single-threaded Sparkplug baseline compilation. Maglev, TurboFan and concurrent compilation remain disabled. `BLINK_JAVASCRIPT_MODE=interpreter` produces a comparison image from the same libraries and scheduling code. For the fixture's fixed 500,000-iteration integer calculation, median elapsed time was 13 ms with the interpreter and 9 ms with Sparkplug. This measures this workload, not every site's JavaScript.

The frame loop schedules animation, lifecycle/paint recording and raster replay as separate tasks. It retains an immutable Chromium PaintRecord and paints into a staging surface. Adaptive 64–256-row strips return to native input polling between tasks. Complete frames are presented atomically; committed navigation discards an unfinished snapshot of the previous document. The raster heuristic targets about 8 ms but cannot bound an individual drawing operation. HTML parsing uses its existing cooperative yield points with a 4 ms UEFI budget. Input queues are drained with bounded counts and time budgets before advancing network work.

`run-qemu.sh` now tries KVM before TCG. KVM device access and VM creation succeeded on this host. Automated tests retain TCG as their default and accept `QEMU_TEST_ACCEL=kvm` for an explicit KVM run.

## Real website results

The tests type `https://zeroday0619.dev` into the address bar. They issue clicks, wheel events, PageDown and address-bar text input while same-origin CSS, fonts and JavaScript are arriving. The loading screenshot shows the typed `x` and the scrolled document before styles finish loading. Theme-button and Space activation are then checked through actual framebuffer color changes after the page handlers are available. No DOM injection or replacement website is used.

| Visible theme response | Previous image, TCG | Final image, TCG | Final image, KVM |
| --- | ---: | ---: | ---: |
| Mouse click | 1346.6 ms | 710.0 ms | 306.8 ms |
| Space key | 878.7 ms | 670.6 ms | 55.7 ms |

These host-side measurements include QMP and screenshot polling, the site's color transition and, for the click, 300 ms of test hold/wait time. They are not optical latency. They are individual paired observations, not a statistical performance guarantee. Network timing and host scheduling can vary between runs.

Maximum recorded input-poll gap was 1242.1 ms before and 1127.4 ms after under TCG. The final KVM site run recorded 82.6 ms. Diagnostics log poll gaps above 50 ms, so these logs must not be used as complete latency distributions. The final TCG maximum individual raster slice was 68.3 ms; complete frame latency can still be much longer while other tasks run between slices.

Screenshots inspected: loading and address input (`heavy-site-loading.png`; local artifact), mouse theme activation (`heavy-site-click.png`; local artifact), keyboard theme activation (`heavy-site-keyboard.png`; local artifact), and scrolling (`heavy-site-scroll.png`; local artifact).

## Controlled load and regressions

`heavy.html` repeatedly runs fixed CPU work and changes the width of an 800-element grid. Five input probes run before that workload stops. Each probe checks the real input event, button handler and document scroll log. The final KVM observations were:

| Input to DOM log | Observed range |
| --- | ---: |
| Keyboard | 5.6–67.2 ms |
| Click, including 30 ms hold | 36.5–72.3 ms |
| Scroll | 62.0–124.0 ms |

The HTTPS fixture withholds its response while inputs are checked. The final KVM run recorded keyboard/click/scroll at 6.0/41.4/41.4 ms before release. Certificate validation remained enabled with a fixture-only CA supplied through `TEST_CA_FILE`; the Mozilla production store was not replaced. Superseding a pending TLS navigation did not commit the stale response.

Form text editing, modifiers, Tab/Space, links, wheel cancellation, nested/document scrolling, back/forward/reload, forward-history truncation and fragment history pass. The 640-movement pointer test checks all four endpoints and absence of cursor trails. Each final QEMU case returns through browser teardown and releases the EFI payload, stack and TLS. Binary validation confirms a static x86-64 EFI executable without required unresolved symbols, PE imports or host syscall instructions. The independent pump-loop regression also passes under sanitizers.

## Reproduction

Run from the project directory:

```sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
python3 tests/virtio-pointer.py
./tests/pump-loop.sh
python3 tests/redirect-server.py > build/heavy-server.log 2>&1 &
server_pid=$!
QEMU_TEST_ACCEL=kvm INPUT_SCENARIO=heavy-fixture python3 tests/browser-qemu.py
INPUT_SCENARIO=heavy-site python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm INPUT_SCENARIO=heavy-site python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm INPUT_SCENARIO=form python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm INPUT_SCENARIO=wheel-fixture python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm INPUT_SCENARIO=history python3 tests/browser-qemu.py
kill "$server_pid"
```

Do not run browser tests concurrently against the same build directory: they share serial-log and OVMF-variable paths. For the interpreter comparison, build with `BLINK_JAVASCRIPT_MODE=interpreter ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/heavy-interpreter" ./build-musl.sh`, then select that `BUILD_DIRECTORY` with the same fixture scenario. The HTTPS procedure uses `tests/https-fixture.py` and the `TEST_CA_FILE`, `TEST_ORIGIN`, `NETWORK_SERVER_LOG` and `NETWORK_RELEASE_FILE` variables documented in `RESULTS.md`.

## Changed files

- `config/blink-firmware.gn`, `build-musl.sh`, `src/blink/browser_entry.cc`: Sparkplug selection, interpreter comparison and bounded native input service before network progress.
- `src/blink/frame_loop.cc`, `frame_loop.h`, `browser_frame.cc`: staged rendering, adaptive raster strips and cancellation on navigation commit.
- `src/blink/message_pump.cc`: long input-poll-gap diagnostics.
- `patches/blink-build.patch`: the UEFI parser budget.
- `patches/edk2-virtio-responsive.patch`, `edk2-virtio-wheel.patch`, `build-input-driver.sh`: report preservation, 5 ms polling and reproducible upgrade from the previous wheel patch.
- `run-qemu.sh`: KVM-first automatic fallback.
- `tests/browser-qemu.py`, `input-scenario.py`, `input-pages/heavy.html`, `virtio-pointer.py`: backend selection, actual-site/load probes and report-queue regression.
- `README.md` and validation artifacts: execution instructions, evidence and limits.

## Remaining limits

JavaScript callbacks, microtask checkpoints, font decoding, lifecycle updates and individual draw operations remain non-preemptible. No DOM callbacks are dispatched from an interrupted JavaScript or layout stack. TCG still recorded a 1.13-second polling gap during cold site loading; this work reduces latency and prevents lost clicks but does not provide hard real-time input. High-refresh physical display latency was not measured. Existing cross-origin/resource-policy and unsupported browser-service limits remain. Human review is still required for the firmware queue and renderer changes.
