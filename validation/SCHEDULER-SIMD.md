# Cooperative EEVDF and presentation SIMD

## Changes

- `src/network/eevdf.h`: weighted virtual runtime, eligibility at the weighted mean virtual time, earliest eligible deadline selection, and slice accounting.
- `src/network/async.c`: interleave suspended requests, preserve per-request errno and guarded stacks, remove completed jobs, and retain abandoned active stacks until firmware completion. Admission remains bounded to 128 jobs.
- `src/blink/navigation.cc`: use weight 4 for main documents; subresources use weight 1. The 500 microsecond target is cooperative and cannot interrupt firmware calls.
- `src/process/entry.S`: SSE2 RGB conversion plus aligned BGR/RGB streaming stores with scalar prefixes/tails and a completion fence. Existing isolated AP processes perform the work.
- `src/process/process.c`: include nonzero reserved bytes in the firmware output self-test.

The scheduler uses the eligibility and virtual-deadline principles described in the [Linux EEVDF documentation](https://docs.kernel.org/scheduler/sched-eevdf.html). It is an original cooperative request-queue implementation, not Linux scheduler code or a CPU process scheduler. Blink's scheduler is unchanged.

## Results

A deterministic server holds one response for three seconds while another response is immediately available. On QEMU/KVM with four vCPUs, the baseline fast response completed after 3030 ms; the new queue completed it after 422 ms. Slow responses completed after 3018 and 3014 ms respectively. Mouse click and keyboard input were acknowledged before the slow response completed. This demonstrates removal of head-of-line blocking; it does not isolate EEVDF's benefit from concurrent dispatch or establish a general website speedup.

The real zeroday0619.dev navigation, scrolling, tab switching, surviving-tab interaction, and firmware cleanup passed. Three isolated AP workers completed 223 presentations with zero failures. The longest observed network dispatch was 13.834 ms. The longest logged input poll gap was 137.792 ms, so synchronous JavaScript and lifecycle work still cause input latency.

The one-, two-, and four-vCPU presentation tests produced byte-identical visible framebuffers and passed isolation, guard, and teardown checks. Four frame presentations totaled 3634, 4551, and 4344 microseconds respectively. This small workload does not show a multiprocessing speedup. SSE2 correctness was additionally checked on the host against scalar expected pixels for 4128 combinations of width, alignment, stride, and pixel format. SIMD throughput was not separately benchmarked.

## Reproduction

Run from the project directory:

```sh
./tests/network-scheduler.sh
./tests/pump-loop.sh
python3 build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/blink" ./build-musl.sh
python3 tests/redirect-server.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=parallel python3 tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_CPUS=4 INPUT_SCENARIO=tabs-site python3 tests/browser-qemu.py
python3 tests/process-qemu.py
BUILD_DIRECTORY="$PWD/build/blink" python3 validate-binary.py
```

Keep the fixture server running in another terminal. Run QEMU tests sequentially because they share the build ESP. The host scheduler test checks eligibility, weighted fairness, admission, slice overrun, errno isolation, early completion, and cancellation. The assembly coroutine test uses UBSan; the pure scheduler uses ASan and UBSan. Assembly memory accesses are checked through explicit output guards, not sanitizer instrumentation.

Evidence: scheduler-results.json, scheduler-baseline.log, scheduler-parallel.log, scheduler-site.log, scheduler-binary.log, and process-comparison.json. Existing process-Ncpu.log files contain the CPU-count runs. These results do not establish physical-hardware behavior or complete renderer process separation.

The one-vCPU concurrent request test also passed: fast=474 ms, slow=3012 ms, with click and keyboard input before slow completion. See scheduler-parallel-1cpu.log.
