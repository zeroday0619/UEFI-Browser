# UEFI browser implementation goals

The active objective includes all nine requirements below. Passing one subsystem test does not complete the objective. The current tree embeds Chromium components; it is not a complete Chrome application or an independently audited sandbox.

| Requirement | Current evidence | Remaining acceptance work |
| --- | --- | --- |
| Complete sandbox | CPL3 Blink/V8 execution, supervisor-owned page tables and fixed broker, negative access tests and recovery, plus alias-wide JIT write/execute exclusion and TLS-scoped write transitions. See `RENDERER-CPL3.md`, `JIT-WX.md` and `SCROLL-WIRE.md`. | Audit every trusted interface and shared-memory consumer; separate privileged web services and sites; extend JIT lifetime and concurrency coverage; validate lifetime/race/alias boundaries, adversarial resource use and independent review. |
| Dynamic memory allocation | Firmware-backed C allocations and sparse virtual commitments already exist. The current change adds an available-memory-derived C heap budget, empty-slab reclamation, allocator failure semantics and physical-page broker quota accounting. | Validate sustained memory pressure, fragmentation, allocation fairness, process quotas and exhaustion/recovery across all services. Empty renderer page batches now return to the broker. Supervisor page tables grow through a bounded BSP reserve; see `PAGE-RECLAMATION.md`. Memory-pressure notifications reclaim physical backing, and identity-only grants skip an unnecessary full-arena revocation walk; see `MEMORY-PRESSURE.md` and `GRANT-REVOCATION.md`. Ordered gap lookup removes repeated full scans, and the reservation registry now grows beyond 4,096 entries; see `ORDERED-RESERVATIONS.md` and `DYNAMIC-REGIONS.md`. BSP allocation ownership/overlap queries now use an ordered index without relaxing page checks; see `BROKER-ALLOCATION-INDEX.md`. Sustained live-site pressure and input responsiveness remain under investigation. |
| Complete Chromium UEFI port | Selected Blink/V8/Skia, network, storage, workers, media and software GPU paths run in QEMU. | Close every gap in `CHROMIUM-PORT-STATUS.md`; connect remaining browser/device services and run relevant Chromium, WPT and GPU CTS coverage. Linked libraries or exposed JavaScript names do not establish implementation. |
| Korean and Japanese input methods | libhangul and Mozc composition are connected to the address bar and Blink. Real Mozc dictionary conversion passed in CPL0 and CPL3. QEMU verified candidate clicks, page/contenteditable/iframe input, native address input and the Korean regression; see `CJK-INPUT.md`. | Broader focus, script-interaction and input-event conformance; Japanese multi-segment/caret/selection behavior, physical keyboards, accessibility, user dictionaries and preference persistence. Native address shaping and page-input latency during long renderer tasks also remain incomplete. |
| Fatal error display | Current change adds an allocation-free-in-the-caller UEFI console display for privileged `abort()`, with diagnostic registers and recent serial output. | Integrate supervisor CPU exceptions and startup fatal failures, retain useful fault-time register/stack data, test recursive failure and display fallback. Recoverable renderer faults must remain confined and recoverable. |
| Boot POST screen | Loader displays actual CPU, writable-memory, driver, ELF and payload setup results. Successful display, malformed-ELF rejection and subsequent browser operation passed QEMU verification in `BOOT-POST.md`. POST now disables the firmware boot watchdog before payload setup. Host error-path tests and QEMU success/malformed-ELF checks passed; see `WATCHDOG-LIFETIME.md`. | CPU/memory/driver failure injection and physical-hardware behavior remain unverified. |
| Developer tools | Original Chromium frontend and Blink Inspector sessions are embedded in the default image. QEMU verified Console DOM/CSS mutation, request tracing, debugger pause/step/resume, reopening and target detachment. Sources showed the paused call frame and the line advancing after step-over. A later isolated image completed Console keyboard input on live YouTube; see `BROKER-ALLOCATION-INDEX.md`. See `DEVELOPER-TOOLS.md` for the default image. | Verify arbitrary breakpoints, step-in/out, profiling and broader frontend workflows; connect worker targets, browser domains, source-map retrieval and persistent preferences. |
| YouTube playback | Local media fixtures cover selected WebM and audio paths; original MSE SourceBuffers and ChunkDemuxer now pass VP8/VP9 video and Opus audio streaming, seek and synchronized playback checks, including actual captured AC’97 stereo output. See `MEDIA-SOURCE.md` and `MSE-AUDIO.md`. An earlier live image reached video and PCM output but subsequently showed a player error. The current response-policy image receives the 10.9 MB main bundle and shows changing video frames and a populated YouTube page at 180 seconds, with captured AC’97 PCM. A later broker-index image also returned a live Console player-state query after the 180-second observation. The zero-rate DMA fix passes local MSE controls and audio regressions. Disabling the firmware boot watchdog allowed a 492-second live run to complete Console-mediated pause, resume and seek to 120 seconds. The run still failed real-time progression assertions, with measured rates of 0.196 and 0.323 times the page clock. A subsequent callback-path optimization improves a controlled continuous-paint audio interval from 0.879 to 1.001 times elapsed time. Its 486-second live run again completes pause/resume/seek, but control-interval playback rates of 0.532 and 0.637 still fail the real-time checks. Continuous real-time playback, pointer interaction with live player controls and synchronization remain unverified. Bounded packet batching subsequently passes the queued-work regression, but a 497.66-second live retry still measures playback ratios of 0.752 and 0.715. Pause, resume and seek complete. Opt-in scheduler task attribution now identifies long callbacks without claiming independent media execution. Inspector command attribution subsequently distinguishes a 13.27-second V8 enable phase from JIT gate overhead during Console evaluation. Avoiding CR3 reloads on unchanged JIT modes reduces a controlled nested-scope benchmark by 39.55% while retaining 14 CPL3 boundary checks; see `INSPECTOR-COST.md`. A protected JIT PTE index then reduces a controlled 66-alias mode-transition benchmark from 14.508 ms to 0.830 ms per 1,000 pairs, with capacity fallback and 14 CPL3 boundary cases passing. Its 498.46-second full live playback run completes pause/resume/seek but fails real-time gates at ratios 0.486 and 0.553; see `JIT-INDEX.md`. This does not establish sustained real-time playback. A live diagnostic exposed the two-second renderer shutdown deadline; acknowledged bounded teardown subsequently passed a 197.97-second live check and completed cleanup in 2.27 seconds without NMI. An infinite-script shutdown still forces termination in 2.10 seconds. See `RENDERER-TEARDOWN.md`. See `YOUTUBE.md`, `WATCHDOG-LIFETIME.md`, `AUDIO-DISPATCH.md`, `MEDIA-BATCHING.md` and `MEDIA-TASK-DIAGNOSTICS.md`. | Connect the remaining web-platform, codec, buffering and network behavior required by the live player; verify visible moving video, audible output, seeking and sustained playback in QEMU. |
| Hardware acceleration and UEFI GPU drivers | Optional CPL3 Mesa Venus browser passes WebGL 1/2, WebGPU compute/rendering, canvas and repeated coherence checks on RTX 5080 through a protected transport broker. Concurrent contexts and eight WebGPU/WebGL2 navigation cycles pass with complete cleanup. [Evidence](VENUS-MULTICONTEXT.md). | Add Skia GPU compositing, direct presentation, device-loss recovery and broader hardware/conformance coverage. |

## Current validation boundary

`DYNAMIC-MEMORY.md` records allocator and fatal-console changes, actual commands, image identities and test outcomes. Physical hardware and broad web-platform conformance remain unverified. These goals remain active.

`SCROLL-ENCODING.md` records cooperative picture encoding, a controlled timer
fairness comparison and passing CPL3 scroll/input regressions. Its full live
YouTube run still fails both real-time playback gates despite working pause,
resume and seek. Synchronous painting and scroll-scene rejection remain under
investigation. This progress does not complete any of the nine requirements.

`SHARED-PAINT.md` records removal of the duplicate viewport/scroll paint pass,
exact pixel comparisons for the effect/iframe fixture and passing CPL3 scroll
regressions. The live scene first rejects a nontranslation transform. The
487.07-second live run completes controls and produces nonzero stereo PCM but
fails real-time playback at 0.684 and 0.731. The nine-goal scope remains active.

`AFFINE-SCROLL.md` records 2D affine scroll composition, validated wire version
3 and a renderer input queue fix that prevents later absolute scroll updates
from overtaking earlier wheel events. CPL3 QEMU fixtures verify transformed
clips, sticky content, scrollbar controls and reconciliation during a 20-second
script. The candidate does not replace the default image. Hardware acceleration,
full sandbox auditing and the other acceptance gaps above remain open.
The full 501.79-second live retry now first rejects `clip-filter`; both playback
speed gates still fail at 0.679 and 0.707 despite working pause/resume/seek and
recorded stereo PCM. This is a measured remaining failure, not completion of
the YouTube requirement.


`SCROLL-FILTERS.md` records validated CSS filter transport (wire version 4),
correct filter/output-clip ordering and effect-local save-layer bounds. The
20-second-script fixture and six scroll/media regressions pass at CPL3. A
sequential single-run comparison reduces first observed filtered scrolling from
135.71 ms to 37.41 ms without material pixel changes. The full 500.10-second
YouTube retry no longer rejects the live filter scene and completes pause,
resume and seeking with nonzero stereo PCM. Initial playback still fails the
real-time gate at 0.773; resume reaches 0.962 and the post-seek interval reaches
1.000. Sustained playback, audiovisual synchronization, live blocking-wheel
routing, capture/encoding cost and all broader acceptance gaps remain open.
The candidate is isolated in `build/scroll-filter-bounds`; the default image is
unchanged. None of the nine objectives is declared complete by these checks.


[Cooperative scroll tile encoding](SCROLL-WIRE-SLICES.md) passes seven CPL3
scroll/media/DevTools regressions and improves timer fairness in the controlled
encoding fixture. The corrected live harness completes pause/resume/seek in
494.02 seconds with video and nonzero stereo PCM, but initial and resumed
playback rates remain below the real-time gate at 0.811 and 0.862. Capture and
picture serialization still block the renderer. The default image is unchanged;
sustained playback and all broader acceptance gaps remain open.


[Immutable drawing record reuse](RECORD-REUSE.md) removes redundant wrapper
allocation for matching single-drawing groups. A 600-panel controlled fixture
reduces recorded median capture from 35.260 to 22.847 ms, while timer p95 and
frame count remain unchanged. Seven CPL3 scroll/media/DevTools regressions pass
and five filter frames match the earlier image pixel-for-pixel. The first full
live run stopped at a false-positive Console readiness check. The corrected
`scroll-record-reuse-youtube-retry` completed playback control measurements in
499.14 seconds at CPL3 without renderer faults. Initial playback advanced at
0.826 of wall time, resumed playback at 0.858 and post-seek playback at 0.832.
Pause held and seeking completed without a media error, but both real-time
acceptance gates failed. No completion of the nine objectives is claimed.


[Eager V8 source positions](EAGER-SOURCE-POSITIONS.md) adds an opt-in build mode
that reduces the measured live Debugger.enable command from 18.423 to 0.157
seconds while retaining debugger regression coverage. Overall Console readiness
did not improve, and startup/memory costs are not characterized. The independent
visible-video interval still fails at a 0.847 playback ratio on the prior image.
Neither sustained media playback nor the broader nine objectives is complete.

[BSP-owned audio buffering](AUDIO-RESERVOIR.md) separates retained PCM from
the hardware DMA window and refills it while the renderer is blocked. A
1.5-second JavaScript fixture passes with host-timed PCM signal observation;
the prior image exhibits an 872 ms signal gap in the same measurement.
Six CPL3 audio/media regressions pass. The 671.35-second live YouTube run
passes pause/resume/seek and the visible-interval rate gate at 0.922, but
initial playback still fails at 0.892. Independent media execution,
presentation and audiovisual synchronization remain open. The default image
is unchanged and none of the nine requirements is declared complete.

[The independent Opus service](MEDIA-PROCESS.md) now executes real decoder
calls on a second CPL3 AP with separate writable data, TLS, page tables and
exception state. QEMU verifies two 100-packet decode/fault/restart cycles
during a 12-second Blink JavaScript task, five denied capabilities, immediate
shutdown, surviving page input, and the existing 14 renderer boundary cases.
Real mono/stereo MSE Opus packets now cross the syscall broker to that process
when `ENABLE_MEDIA_SERVICE=1`. Chromium retains timestamp/discard processing;
the BSP owns eight stream handles, bounded copied results and teardown. QEMU
passes MSE audio/seek, zero-rate/resume and timed PCM reservoir tests with at
least 100 production packets per case. Packet submission remains synchronous
and renderer-driven. Independent compressed-data feeding, video presentation
and live YouTube performance remain incomplete. The service remains disabled
by default; none of the nine requirements is declared complete.

The MSE service integration also exposed a recovery mailbox bug: an inactive
recovered tab retained the shared scroll publication, which prevented updates
from other tabs and could leave Reload without a target URL. Recovered tabs
now discard and acknowledge those publications. QEMU passes the full
termination/exception/reload sequence with six CPL3 renderer starts and six
sets of media boundary checks. See `MEDIA-PROCESS.md` for retained failure and
final-success records. This does not complete the overall sandbox goal.

The optional media service now has bounded encoded-audio queues and BSP-side
Chromium timestamp/discard processing. A sequential QEMU comparison during a
six-second script improves the non-silent PCM/wall-clock ratio from 0.332 to
0.999 and reduces the maximum signal gap from 3.872 seconds to 40.67 ms.
Decode-count markers prove new packets were processed during the block;
pause/resume/seek also pass. The renderer still supplies compressed data and
presents video, so this does not establish sustained live YouTube playback.
See `MEDIA-QUEUE.md`. All nine overall requirements remain open.

The queued-service live YouTube run obtains initial/resumed playback ratios
of 0.95569/0.96011, a stable pause and a completed 120-second seek without
media errors. The overall test still fails when the independent visible-tab
measurement does not return. Listener-installation acknowledgement and timeout
screenshots are added for the next live check. A separate reproduced MSE
format-transition bug discarded the entire first mono segment while reporting
normal playback. Draining compressed and pending PCM work before replacing the
decoder restores both eight-second segments in captured output. The three-case
configuration/blocked-script/audio regression passes with CPL3 and broker
boundary probes. See `MEDIA-CONFIG-TRANSITION.md`. These results do not complete
the sandbox, full Chromium compatibility, audiovisual synchronization or hardware
acceleration objectives.

The subsequent configuration candidate completes the live YouTube scenario in
700.55 seconds with all twelve playback checks passing. A continuously visible
20.0116-second interval advances 20.01754 seconds of media time and decodes 612
new frames. CPL3 remains active with no unexpected renderer fault. The visible
screenshot and AC97 PCM capture are preserved. This establishes the measured
interval and tested controls, not audiovisual synchronization or complete
YouTube compatibility. See `MEDIA-CONFIG-TRANSITION.md`.

Mesa Venus 26.2.5 and the required common code are approved. The standalone
UEFI application now uses the official generated command protocol, host-visible
shared memory and separate CPU/GPU completion timelines. The final QEMU run
queries the RTX 5080 and executes two timestamp commands on its graphics queue
in each of three device/context lifecycles. All 65,643 fences, queue wrap and
reset cleanup pass. ASan/UBSan reject 13 malformed stream/handle cases. This is
GPU queue execution, not shader rendering or browser acceleration. Complete
runtime, image/presentation paths, protected broker integration and fault
recovery remain open; see `VIRTIO-GPU.md`.

The next standalone Venus image executes vertex and fragment shaders on the
RTX 5080, rasterizes an interpolated triangle and exports a Vulkan readback
allocation through a mapped HOST3D blob. Three resource lifecycles pass guest
pixel checks and an independent QMP screenshot check. The image is displayed
through a separate standard VGA GOP framebuffer; 65,766 fences and reset/DMA
cleanup pass. See `VENUS-RENDERING.md`. This establishes the tested hardware
rendering and preview path, while browser integration, general resource
management, protected GPU services and full GPU conformance remain open.

The optional Mesa Venus 26.2.5 runtime now links into a separate firmware
musl EFI candidate with a concrete Virtio transport. Native C11 mutex selection
and the missing monotonic sleep syscall were fixed after QEMU reproduction.
Three QEMU lifecycles now pass actual instance/device creation, GPU buffer
fill and timestamp readback, shader triangle rendering, mapped-image readback,
independent GOP presentation and reset/DMA cleanup. A QMP screenshot passes
an independent pixel check. Backend and transport fault tests pass ASan/UBSan.
See `VENUS-RUNTIME.md` for that earlier standalone result. The subsequent
CPL3 browser connects ANGLE/Dawn through a protected transport broker and
preserves device cache attributes. WebGL 1/2, WebGPU, canvas and 32 coherence
cycles pass. `VENUS-BROKER.md` records the initial integration. The newer
`VENUS-MULTICONTEXT.md` records concurrent contexts, eight WebGPU/WebGL2
navigation cycles and cleanup of retained Dawn callbacks. Skia GPU integration, device recovery and broader
hardware/conformance testing remain open.
All broader implementation objectives remain open.
