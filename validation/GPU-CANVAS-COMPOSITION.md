# WebGL and WebGPU canvas composition without presentation readback

## Implementation

The `build/canvas-direct` candidate connects ANGLE WebGL 1/2 and Dawn WebGPU canvas images to the BSP Skia Vulkan compositor. It builds on [GPU composition and presentation](GPU-COMPOSITION.md). Normal supported canvas presentation no longer downloads pixels into a guest CPU bitmap or embeds those pixels in the scroll-scene wire message. Context initialization also publishes the cleared WebGL image before an asynchronous script can yield to the first paint. This removes the two initial reads observed before the first drawing task.

The producer copies the resolved canvas into an immutable device-local Vulkan image. A completed producer fence and external queue-family release precede publication. The Venus broker validates the allocation owner, format, dimensions, size and quota, then issues a generation-tagged image capability. Scroll-wire version 5 carries that token and existing transform, clip and effect metadata. It does not carry a renderer address, Vulkan handle or serialized Skia object.

The BSP imports the retained Venus resource into its own Vulkan device and wraps the image in a borrowed Skia texture. Scene references and Skia release callbacks retain the image until it is no longer used. Repeated publications of the same token share an import. Context close removes producer ownership without invalidating an outstanding consumer reference. The transport detaches consumer contexts before releasing the final resource reference.

Supported exported formats are RGBA8, BGRA8 and RGBA16Float. Image metadata preserves alpha mode, origin and Blink color space. Opaque WebGPU canvases run Chromium's GPU alpha-clear pass before export. This replaces stored alpha values with one while preserving RGB; an opaque Skia metadata hint alone did not prevent background-color leakage. The broker limits images to 4096 × 4096 pixels, 64 live capabilities and 128 MiB of retained image allocations. Vulkan memory requirements are checked again before binding an imported allocation. The scene decoder rejects unknown or stale image tokens.

Explicit pixel APIs remain available. WebGL `readPixels`, canvas snapshots and WebGPU buffer readback still perform the requested reads. WebGPU retains a private GPU front texture after the script-visible current texture expires so later snapshots preserve the presented contents. Unsupported export configurations or producer export failures use the existing CPU snapshot path.

This is not a zero-copy or fully asynchronous pipeline. Producer image copies, fence waits and per-frame allocation remain. WebGPU also copies its retained front texture. Normal DOM tiles still use renderer CPU rasterization and bounded pixel publication. Consumer import failure and device loss do not have complete recovery paths. Canvas image drawing currently uses the scroll compositor's nearest-neighbor sampling; CSS interpolation-quality parity, OffscreenCanvas transfer paths and HDR output are not established by these tests. The existing Venus command-stream and host-driver trust boundary is unchanged; image capability validation is not a new complete GPU sandbox.

## Changed components

| Component | Change |
| --- | --- |
| `src/blink/gpu_canvas.{cc,h}` | Immutable GPU export, retained image leases and repeated-import reuse |
| `src/blink/gpu_compositor.{cc,h}` | Venus allocation import and Skia texture lifetime |
| `src/blink/scroll_scene.cc`, `scroll_wire.{cc,h}`, `scroll_snapshot.h` | Ordered canvas layers, version 5 image tokens and token rejection |
| `runtime/venus_image.h`, `venus_broker.h`, `venus_device.h`, `venus_transport.h` | Bounded image metadata and capability/attachment interfaces |
| `src/gpu/venus-runtime/{broker,proxy,renderer,transport}.c` | Ownership checks, resource pinning, consumer attachment and teardown |
| `src/blink/{gpu_context,webgl_context}.cc`, `patches/blink-{build,angle,dawn}.patch` | Blink canvas integration and validated ANGLE/Dawn texture export |
| `patches/mesa-venus-uefi.patch` | Private Venus memory-resource import |
| `build-musl.sh`, `config/blink-navigation.gn` | Feature control and build integration |
| `tests/input-pages/gpu-canvas-*.html`, `tests/input-scenario.py`, `tests/state-suite.py` | A/B fixture, visible pixel checks and live-site scenarios |
| `tests/browser-qemu.py`, `tests/qemu_test_desktop.py` | Bounded SDL capture retries and inactive KDE test-desktop placement |
| `tests/venus-{broker,runtime,transport}-host.c` | Ownership, malformed metadata, retained references and transport cleanup checks |

## Build and reproduce

The tested stack is QEMU 11.1.2 with the project scanout patch, virglrenderer 1.3.0, Mesa Venus 26.2.5, NVIDIA 615.78.08 and an RTX 5080. QEMU uses KVM, four vCPUs, 4 GiB RAM, a 512 MiB host-visible GPU aperture and a 1280 × 800 SDL/X11 EGL window. The default `build/blink` image is not replaced.

```sh
python3 build-venus-runtime.py --jobs 4
python3 build-blink-libraries.py --mozc --jobs 4
ENABLE_BLINK=1 ENABLE_VENUS_GPU=1 ENABLE_GPU_COMPOSITION=1 \
  ENABLE_CPL3_PROBE=1 ENABLE_BROKER_PROBE=1 JOBS=4 \
  BUILD_DIRECTORY="$PWD/build/canvas-direct" ./build-musl.sh

# This control retains Skia GPU composition but disables direct canvas import.
ENABLE_BLINK=1 ENABLE_VENUS_GPU=1 ENABLE_GPU_COMPOSITION=1 \
  ENABLE_GPU_CANVAS=0 ENABLE_CPL3_PROBE=1 ENABLE_BROKER_PROBE=1 JOBS=4 \
  BUILD_DIRECTORY="$PWD/build/canvas-readback-control" ./build-musl.sh

# This manual launch opens an interactive window; automated tests below are isolated.
BUILD_DIRECTORY="$PWD/build/canvas-direct" QEMU_GPU_COMPOSITION=1 \
  QEMU_ACCEL=kvm VK_DRIVER_FILES=/etc/vulkan/icd.d/nvidia_icd.json \
  ./run-qemu.sh
```

`ENABLE_GPU_CANVAS` defaults to 1 in GPU-composition builds. It does not enable the Venus backend or replace software rendering in default builds.

```sh
python3 tests/network-servers.py --directory build/canvas-direct-https
# Run tests in another terminal while the server is active.
QEMU_BINARY="$PWD/build/qemu-scanout/qemu-system-x86_64" \
  QEMU_TEST_VENUS=1 QEMU_TEST_GPU_COMPOSITION=1 \
  QEMU_TEST_REQUIRE_GPU_CANVAS=1 \
  VK_DRIVER_FILES=/etc/vulkan/icd.d/nvidia_icd.json \
  python3 tests/state-suite.py --workers 1 --cpus 4 \
  --https-ca build/canvas-direct-https/ca.esl \
  --build-directory build/canvas-direct --prefix canvas-local-check \
  --require-cpl3-probe --require-broker-probe --require-scroll-wire \
  --require-no-renderer-faults \
  webgl2 webgpu-canvas gpu-canvas-colors gpu-canvas-perf \
  gpu-multicontext gpu-lifecycle scroll-input heavy-site

python3 tests/venus-transport-host.py
python3 tests/venus-runtime-host.py
python3 tests/venus-broker-host.py
```

For the control, select `build/canvas-readback-control` and omit `QEMU_TEST_REQUIRE_GPU_CANVAS`. Real HTTPS sites use the Firefox CA store; the fixture CA applies only to local test pages. Live-site scenarios are environment-dependent and run separately from the deterministic default suite.

## Measurement boundaries

The animation fixture renders WebGL2 and WebGPU canvases at 960 × 560 pixels each for at least 15 seconds. Its frame intervals are JavaScript `requestAnimationFrame` intervals, not optical frame times. Input measurements run from host QMP delivery through the page's console handler log. Host scheduling, QMP and a 10 ms log polling interval contribute to those values. They do not measure input-to-photon latency.

Readback counters are recorded at WebGL's presentation/snapshot framebuffer read and WebGPU's CPU snapshot transfer. Direct JavaScript `gl.readPixels` calls are separate explicit API operations and are not part of this counter. The performance fixture does not request explicit pixel reads. Byte counts describe requested readback payloads, not measured PCIe traffic. Host SDL screenshots read pixels only for test evidence and are outside the guest canvas pipeline.

Exploratory runs used incomplete WebGL readback instrumentation or preceded the initial-image fix and are excluded from final comparison. Final evidence uses the `canvas-finalized-` prefix after the opaque-alpha fix. Earlier `canvas-release-` results are retained as intermediate evidence. The final comparison uses the corrected instrumentation in both binaries and alternating execution order without concurrent compilation or other test VMs.

## Controlled results on 2026-10-21 (Asia/Seoul)

Both EFI images link the same native Blink/ANGLE/Dawn/Skia libraries and Mesa archive. Only `ENABLE_GPU_CANVAS` differs. [The manifest](canvas-finalized-manifest.json) records the image and source hashes. Three runs per mode passed with the order control/direct, direct/control, control/direct. These final runs followed temporary-disk cleanup and did not overlap compilation or another test VM.

Values below are medians across the three runs. The p95 row is the median of each run's p95, not a pooled percentile. [Raw runs and aggregates](canvas-finalized-comparison.json) retain all individual input samples.

| Measurement | CPU canvas readback control | Direct GPU canvas |
| --- | ---: | ---: |
| Animation frames in approximately 15 seconds | 100 | 124 |
| Median JavaScript frame interval (ms) | 147.2 | 118.9 |
| p95 JavaScript frame interval (ms) | 158.5 | 125.4 |
| Fixture initialization to first drawing callback (ms) | 518.5 | 549.1 |
| Requested presentation/snapshot readback payload (bytes) | 645,120,000 | 0 |
| QMP click to page-handler log (ms) | 32.22 | 63.18 |
| QMP keyboard input to page-handler log (ms) | 41.75 | 42.16 |
| QMP wheel to page scroll-handler log (ms) | 53.24 | 21.91 |

The median frame interval decreased by 19.2% and the p95 interval decreased by 20.9%. Canvas readback was absent from all three direct runs, including initial context creation. The fixture initialization time did not improve. The click median increased, keyboard latency was nearly unchanged and the wheel-to-scroll-handler median decreased. With three samples per event type and phase-dependent results, these measurements do not establish a general responsiveness improvement. Producer fence waits, per-frame image allocation/import and remaining tile rasterization are still optimization targets. These results do not isolate their individual contribution to input delay.

Every completed benchmark passed the 14 CPL3 boundary probes, the existing 65-check broker probe, the scroll-wire self-test and context accounting. All three GPU contexts closed with none remaining and no unexpected renderer fault. The scroll-wire self-test includes rejection of an unknown image capability.

## Failed exploratory checks

The external [WebGL Aquarium](https://webglsamples.org/aquarium/aquarium.html) did not initialize a GPU context within 150 seconds after its HTTPS scripts were fetched. The cause remains unverified. This attempt is retained in `canvas-direct-visual-gpu-aquarium.log` and is not counted as a passing real-site test. An earlier attempt also used the local fixture CA for that public site; the harness now keeps those trust stores separate. The Aquarium scenario remains opt-in.

The readback control rendered a blank canvas on [WebGL Fundamentals 3D Perspective](https://webglfundamentals.org/webgl/webgl-3d-perspective.html) and failed its visible geometry-change assertion. The direct candidate rendered the geometry and responded to the slider and keyboard. The failed control run is retained under `canvas-verified-live-control`; it cannot supply a valid visual latency comparison for that page.

Intermediate direct runs exposed two corrected defects: an initialized WebGL canvas performed two reads before its first drawing task, and opaque WebGPU pixels with stored alpha zero leaked the background color. The final tests include initialization-time readback rejection and opaque-alpha pixel checks over a colored background for all three supported WebGPU formats.


## Final functional validation on 2026-10-21 and 2026-10-22

| Validation | Result and evidence |
| --- | --- |
| WebGPU API, alpha/origin/format pixels and repeated context switching | All three cases passed in [the alpha/lifecycle suite](canvas-finalized-alpha-lifecycle-suite.json). Pixel checks cover 14 locations across 10 canvases. Lifecycle checks include eight WebGPU/WebGL2 round trips. |
| WebGL2, WebGPU snapshots/resize, multiple contexts and long-JavaScript scrolling | All four cases passed in [the regression suite](canvas-finalized-regressions-suite.json). Explicit readback APIs remain functional. The scroll case includes 12 seconds of blocking JavaScript. |
| `https://zeroday0619.dev` | Passed in the regression suite and [the control run](canvas-finalized-real-control-suite.json). Input was delivered during loading; theme clicks, keyboard input and wheel scrolling were exercised after load. |
| WebGL Fundamentals 3D Perspective | Passed in [the final live-site retest](canvas-finalized-live-3-suite.json). Slider and Home-key interactions changed 2,603 and 5,205 canvas pixels, respectively; the canvas readback counter remained zero. |
| Broker, runtime and transport host tests | `python3 tests/venus-broker-host.py`, `python3 tests/venus-runtime-host.py` and `python3 tests/venus-transport-host.py` passed with sanitizers. Logs use the `canvas-direct-*-host.log` names. |
| Patch reproducibility and builds | Chromium patches applied to the pinned base and reverse checks passed against the modified checkout. Mesa patch checks, native library builds and both final EFI links passed. See `canvas-direct-patch-checks.log`, `canvas-direct-opaque-alpha-build.log`, `canvas-direct-final-efi-build.log` and `canvas-readback-control-final-efi-build.log`. |

The single zeroday0619.dev control/direct observations do not establish a loading or input-latency improvement. Visible theme-click response was 1,721.25/2,154.60 ms and keyboard response was 2,052.68/1,974.23 ms. This page did not publish GPU canvas images in these runs. Those observations must not be presented as a benefit of direct canvas composition.

The regression suite's original live-WebGL case failed because the test assumed an initial pointer position and clicked above the slider. The test now homes the guest pointer through QMP before moving to a page control. A later retest failed in host SDL image capture before navigation; bounded retries now re-enumerate the window and remove stale output before each capture. The failed records remain in `canvas-finalized-regressions` and `canvas-finalized-live-2`; the passing `canvas-finalized-live-1` and `canvas-finalized-live-3` runs are separate records.

## Isolated automated display

GPU screenshot tests now call `tests/qemu_test_desktop.py` before launching SDL. On KDE Plasma, this creates or reuses the `UEFI QEMU Tests` virtual desktop without switching the current desktop. A persistent KWin rule matches only the `uefi-browser-test` window class. It forces that desktop, rejects focus and excludes test windows from the taskbar and task switcher. QMP delivers guest input without moving the host pointer. Audio remains disabled by the existing harness unless an explicit WAV capture is requested.

The helper preserves existing window rules and saves the original configuration as `~/.config/kwinrulesrc.before-uefi-tests` on first use. It refuses to launch an SDL test if the desktop cannot be prepared or is currently active. KDE's `qdbus6`, `kreadconfig6` and `kwriteconfig6` are required for this automated SDL path; there is no silent fallback to the active desktop. Headless tests are unchanged. The manual `run-qemu.sh` entry point remains interactive.

This isolates window placement and focus, not CPU or GPU resources. Tests and games still share the host GPU. Measurements collected while gaming are functional checks and must not be mixed into the controlled performance comparison above. Desktop verification uses KWin's actual window state; this host's XWayland root reports one X11 desktop regardless of the KDE virtual-desktop assignment. The rule fields follow [KWin's rule schema](https://github.com/KDE/kwin/blob/master/src/rulesettings.kcfg).

The isolated live WebGL run passed in 42.57 seconds: [suite result](canvas-isolated-desktop-suite.json), [canvas results](canvas-isolated-desktop-gpu-live-webgl-gpu-live-webgl-results.json) and [desktop evidence](qemu-test-desktop-validation.json). KWin reported the test desktop assignment, `active=false` and `wantsInput=false` at window creation. The current desktop remained unchanged. The off-desktop SDL capture showed the rendered geometry and both input-driven changes; readback remained zero. This run is excluded from the performance table.
