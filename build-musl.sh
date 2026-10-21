#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
# Distinct EFI output directories still share the configured musl library.
mkdir -p "$project/build/musl"
exec 9>"$project/build/musl/build.lock"
flock 9
source_tree=$(python3 "$project/prepare-musl.py")
build=${BUILD_DIRECTORY:-"$project/build/musl"}
libc_build="$project/build/musl/libc"
mkdir -p "$libc_build" "$build/esp/EFI/BOOT"
common=(-O2 -fno-builtin -fno-stack-protector -mno-red-zone -fno-pic -fno-pie -ffunction-sections -fdata-sections)
(
 cd "$libc_build"
 "$source_tree/configure" --target=x86_64 --disable-shared --disable-wrapper CC=clang AR=llvm-ar RANLIB=llvm-ranlib "CFLAGS=${common[*]}" > configure.log
 make -j"${JOBS:-8}" lib/libc.a > build.log 2>&1
)
includes=(-nostdinc -isystem "$(clang -print-resource-dir)/include" -I"$source_tree/arch/x86_64" -I"$source_tree/arch/generic" -I"$libc_build/obj/src/internal" -I"$libc_build/obj/include" -I"$source_tree/src/include" -I"$source_tree/src/internal" -I"$source_tree/include" -I"$project/runtime")
# Browser builds fail closed when an unprivileged AP renderer cannot be started.
renderer_cpl3_default=0
if [[ ${ENABLE_BLINK:-0} == 1 || ${ENABLE_VENUS_RUNTIME:-0} == 1 ]]; then renderer_cpl3_default=1; fi
renderer_cpl3=${ENABLE_RENDERER_CPL3:-$renderer_cpl3_default}
case "$renderer_cpl3" in 0|1) ;; *) printf '%s\n' 'ENABLE_RENDERER_CPL3 must be 0 or 1.' >&2; exit 1 ;; esac
engine_flags=()
engine_objects=()
if [[ ${ENABLE_GPU_COMPOSITION:-0} == 1 ]]; then
 if [[ ${ENABLE_VENUS_GPU:-0} != 1 ]]; then
  printf '%s\n' 'ENABLE_GPU_COMPOSITION requires ENABLE_VENUS_GPU=1.' >&2
  exit 1
 fi
 engine_flags+=(-DENABLE_GPU_COMPOSITION)
 case ${ENABLE_GPU_CANVAS:-1} in
  1) engine_flags+=(-DENABLE_GPU_CANVAS) ;;
  0) ;;
  *) printf '%s\n' 'ENABLE_GPU_CANVAS must be 0 or 1.' >&2; exit 1 ;;
 esac
fi
if [[ ${ENABLE_VENUS_RUNTIME:-0} == 1 || ${ENABLE_VENUS_GPU:-0} == 1 ]]; then
 if [[ $renderer_cpl3 != 1 ]]; then
  printf '%s\n' 'Mesa Venus requires the CPL3 broker for device memory cache attributes.' >&2
  exit 1
 fi
 if [[ ${ENABLE_VENUS_RUNTIME:-0} == 1 && ${ENABLE_BLINK:-0} == 1 ]]; then
  printf '%s\n' 'ENABLE_VENUS_RUNTIME currently requires a standalone GPU probe.' >&2
  exit 1
 fi
 if [[ ${ENABLE_VENUS_GPU:-0} == 1 && ( ${ENABLE_BLINK:-0} != 1 || $renderer_cpl3 != 1 ) ]]; then
  printf '%s\n' 'ENABLE_VENUS_GPU requires the CPL3 Blink browser.' >&2
  exit 1
 fi
 if [[ ${ENABLE_VENUS_RUNTIME:-0} == 1 ]]; then engine_flags+=(-DENABLE_VENUS_RUNTIME -DENABLE_CPP); fi
 if [[ ${ENABLE_VENUS_GPU:-0} == 1 ]]; then engine_flags+=(-DENABLE_VENUS_GPU); fi
 venus_source="$project/build/venus-runtime-source/mesa-26.2.5"
 venus_archive="$project/build/venus-runtime/src/virtio/vulkan/libvulkan_virtio.a"
 venus_api_flags=()
 if [[ ${ENABLE_VENUS_GPU:-0} == 1 ]]; then
  venus_symbols=()
  for symbol in vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion; do
   venus_symbols+=(--redefine-sym "$symbol=uefi_venus_$symbol")
   venus_api_flags+=(-D$symbol=uefi_venus_$symbol)
  done
  llvm-objcopy "${venus_symbols[@]}" "$venus_archive" "$build/libvenus-browser.a"
  venus_archive="$build/libvenus-browser.a"
 fi
 native="$project/../../work/chromium-build/chromium/out/uefi-blink-native/obj/buildtools/third_party"
 venus_names=(transport)
 if [[ ${ENABLE_VENUS_RUNTIME:-0} == 1 ]]; then
  python3 "$project/tools/build-venus-shaders.py" "$build/venus-shaders"
  venus_names+=(probe render)
 fi
 venus_device_flags=()
 if [[ $renderer_cpl3 == 1 ]]; then
  engine_flags+=(-DENABLE_VENUS_CPL3)
  venus_names+=(broker proxy)
  for operation in open close submit poll release_fence create_blob destroy_blob present_image scanout retain_blob release_blob attach_blob detach_blob; do
   venus_device_flags+=(-Dfirmware_venus_$operation=firmware_venus_device_$operation)
  done
 fi
 for name in "${venus_names[@]}"; do
  venus_flags=()
  if [[ $name == transport ]]; then venus_flags+=("${venus_device_flags[@]}"); fi
  clang "${venus_api_flags[@]}" "${engine_flags[@]}" "${venus_flags[@]}" "${common[@]}" "${includes[@]}" -I"$venus_source/include" -I"$venus_source/src" -I"$build/venus-shaders" \
    -c "$project/src/gpu/venus-runtime/$name.c" -o "$build/venus-$name.o"
  engine_objects+=("$build/venus-$name.o")
 done
 engine_objects+=(--whole-archive "$venus_archive" --no-whole-archive "$native/libc++/libc++.a" "$native/libc++abi/libc++abi.a")
fi
media_service=${ENABLE_MEDIA_SERVICE:-${ENABLE_MEDIA_SERVICE_PROBE:-0}}
case "$media_service" in 0|1) ;; *) printf '%s\n' 'ENABLE_MEDIA_SERVICE must be 0 or 1.' >&2; exit 1 ;; esac
if [[ $media_service == 1 ]]; then
 if [[ $renderer_cpl3 != 1 || ${ENABLE_BLINK:-0} != 1 ]]; then
  printf '%s\n' 'ENABLE_MEDIA_SERVICE requires ENABLE_BLINK=1 and ENABLE_RENDERER_CPL3=1.' >&2; exit 1
 fi
 engine_flags+=(-DENABLE_MEDIA_SERVICE)
 if [[ ${ENABLE_MEDIA_SERVICE_PROBE:-0} == 1 ]]; then
  engine_flags+=(-DENABLE_MEDIA_SERVICE_PROBE)
 fi
fi
if [[ ${ENABLE_MOZC_PROBE:-0} == 1 ]]; then
 engine_flags+=(-DENABLE_MOZC_PROBE)
fi
if [[ ${ENABLE_PANIC_PROBE:-0} == 1 ]]; then
 engine_flags+=(-DENABLE_PANIC_PROBE)
fi
if [[ ${ENABLE_MEMORY_PROBE:-0} == 1 ]]; then
 engine_flags+=(-DENABLE_MEMORY_PROBE)
 clang "${common[@]}" "${includes[@]}" -c "$project/src/musl/memory_probe.c" -o "$build/memory-probe.o"
 engine_objects+=("$build/memory-probe.o")
fi
if [[ ${ENABLE_CPL3_PROBE:-0} == 1 ]]; then
 if [[ $renderer_cpl3 != 1 ]]; then
  printf '%s\n' 'ENABLE_CPL3_PROBE requires ENABLE_RENDERER_CPL3=1.' >&2
  exit 1
 fi
 engine_flags+=(-DENABLE_CPL3_PROBE)
 clang "${common[@]}" "${includes[@]}" -c "$project/src/musl/jit_probe.c" -o "$build/jit-probe.o"
 engine_objects+=("$build/jit-probe.o")
fi
if [[ ${ENABLE_BROKER_PROBE:-0} == 1 ]]; then
 engine_flags+=(-DENABLE_BROKER_PROBE)
 clang "${common[@]}" "${includes[@]}" -c "$project/src/process/broker_probe.c" -o "$build/broker-probe.o"
 engine_objects+=("$build/broker-probe.o")
fi
if [[ ${ENABLE_SERVICE_PROBE:-0} == 1 ]]; then
 engine_flags+=(-DENABLE_SERVICE_PROBE)
 clang "${common[@]}" "${includes[@]}" -c "$project/src/musl/io_probe.c" -o "$build/io-probe.o"
 engine_objects+=("$build/io-probe.o")
fi
if [[ ${ENABLE_BLINK:-0} == 1 ]]; then
 engine_flags+=(-DENABLE_BLINK -DENABLE_CPP)
 root=$(cd "$project/../.." && pwd)
 archive="$root/work/chromium-build/chromium/out/uefi-blink-native/obj/third_party/blink/uefi/libbrowser.a"
 if [[ ! -f "$archive" ]]; then
  printf '%s\n' "Blink browser archive is absent. Run build-blink-libraries.py first." >&2
  exit 1
 fi
 cat > "$build/fonts.S" <<ASM
.section .rodata,"a",@progbits
.balign 16
.globl noto_latin_start,noto_latin_end,noto_cjk_start,noto_cjk_end
noto_latin_start:
.incbin "$project/fonts/NotoSans-Regular.ttf"
noto_latin_end:
.balign 16
noto_cjk_start:
.incbin "$project/fonts/NotoSansCJKsc-Regular.otf"
noto_cjk_end:
.section .note.GNU-stack,"",@progbits
ASM
 clang -c "$build/fonts.S" -o "$build/fonts.o"
 python3 "$project/tools/embed-blink-resources.py" "$root/work/chromium-build/chromium" "$build/resources.c"
 clang "${common[@]}" "${includes[@]}" -c "$build/resources.c" -o "$build/resources.o"
 python3 "$project/tools/embed-devtools.py" "$root/work/chromium-build/chromium/out/uefi-blink-native/gen/third_party/devtools-frontend/src" "$build"
 clang "${common[@]}" "${includes[@]}" -c "$build/devtools-index.c" -o "$build/devtools-index.o"
 clang -c "$build/devtools-data.S" -o "$build/devtools-data.o"
 engine_objects+=("$build/devtools-index.o" "$build/devtools-data.o")
 python3 "$project/tools/blink-link-inputs.py" "$root/work/chromium-build/chromium/out/uefi-blink-native" "$build/blink-runtime.rsp"
 python3 - "$build/browser-url.c" <<'PYTHON'
import json, os, pathlib, sys, urllib.parse
url = os.environ.get('BLINK_START_URL', 'about:blank')
parsed = urllib.parse.urlsplit(url)
if url != 'about:blank' and (parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username or parsed.password or len(url) >= 4096):
    raise SystemExit('BLINK_START_URL must be about:blank or an HTTP(S) URL without credentials, shorter than 4096 characters.')
mode = os.environ.get('BLINK_JAVASCRIPT_MODE', 'optimized')
flags = {'optimized': '--single-threaded --sparkplug --turbofan',
         'sparkplug': '--single-threaded --sparkplug --no-turbofan',
         'interpreter': '--single-threaded --jitless'}
if mode not in flags:
    raise SystemExit('BLINK_JAVASCRIPT_MODE must be optimized, sparkplug, or interpreter.')
source_positions = os.environ.get('BLINK_SOURCE_POSITIONS', 'lazy')
if source_positions not in ('lazy', 'eager'):
    raise SystemExit('BLINK_SOURCE_POSITIONS must be lazy or eager.')
javascript_flags = flags[mode] + (' --no-enable-lazy-source-positions'
                                if source_positions == 'eager' else '')
diagnostics = os.environ.get('BLINK_TASK_DIAGNOSTICS', '0')
if diagnostics not in ('0', '1'):
    raise SystemExit('BLINK_TASK_DIAGNOSTICS must be 0 or 1.')
pathlib.Path(sys.argv[1]).write_text('const char firmware_browser_url[] = ' + json.dumps(url) + ';\n' +
    'const char firmware_javascript_flags[] = ' + json.dumps(javascript_flags) + ';\n' +
    'const int firmware_task_diagnostics = ' + diagnostics + ';\n')
PYTHON
 clang "${common[@]}" "${includes[@]}" -c "$build/browser-url.c" -o "$build/browser-url.o"
 engine_objects+=("$archive" "$build/fonts.o" "$build/resources.o" "$build/browser-url.o" "@$build/blink-runtime.rsp")
fi

if [[ ${ENABLE_BLINK:-0} == 1 || ${ENABLE_BLINK_COMPAT:-0} == 1 ]]; then
 engine_flags+=(-DENABLE_BLINK_COMPAT)
 clang "${common[@]}" "${includes[@]}" -D_GNU_SOURCE -c "$project/src/musl/blink_compatibility.c" -o "$build/blink-compatibility.o"
 engine_objects+=("$build/blink-compatibility.o")
fi
if [[ ${ENABLE_NETWORK:-0} == 1 ]]; then
 engine_flags+=(-DENABLE_NETWORK)
 python3 - "$build/network-request.h" <<'PYTHON'
import json, os, pathlib, sys
url = os.environ.get('NETWORK_URL')
pathlib.Path(sys.argv[1]).write_text('#define NETWORK_URL ' + json.dumps(url) + '\n' if url else '')
PYTHON
 for name in resource chunks probe async; do
  clang "${common[@]}" "${includes[@]}" -include "$build/network-request.h" -c "$project/src/network/$name.c" -o "$build/network-$name.o"
  engine_objects+=("$build/network-$name.o")
 done
fi
if [[ ${ENABLE_V8:-0} == 1 || ${ENABLE_CPP:-0} == 1 || ${ENABLE_SKIA:-0} == 1 ]]; then
 engine_flags+=(-DENABLE_CPP)
 root=$(cd "$project/../.." && pwd)
 v8="$root/work/chromium-build/v8"
 engine_sources=("$project/src/musl/cxx_probe.cpp")
 if [[ ${ENABLE_V8:-0} == 1 ]]; then
  engine_flags+=(-DENABLE_V8)
  engine_sources+=("$project/src/v8_probe.cpp" "$project/src/musl/protection.cpp")
 fi
 if [[ ${ENABLE_SKIA:-0} == 1 ]]; then
  engine_flags+=(-DENABLE_SKIA)
  engine_sources+=("$project/src/musl/skia_probe.cpp")
 fi
 for engine_source in "${engine_sources[@]}"; do
  engine_name=$(basename "$engine_source" .cpp)
  clang++ "${common[@]}" -std=c++23 -fno-exceptions -fno-rtti -nostdinc -nostdinc++ \
  -I"$v8/buildtools/third_party/libc++" -isystem "$v8/third_party/libc++/src/include" \
  -isystem "$v8/third_party/libc++abi/src/include" \
  -idirafter "$(clang -print-resource-dir)/include" -idirafter "$libc_build/obj/include" \
  -idirafter "$source_tree/include" -idirafter "$source_tree/arch/x86_64" -idirafter "$source_tree/arch/generic" \
  -DANDROID_HOST_MUSL -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE \
  -I"$v8/include" -I"$v8/out/musl/gen/include" -I"$root/work/chromium-build/skia" -I"$project/runtime" \
  -c "$engine_source" -o "$build/$engine_name.o"

  engine_objects+=("$build/$engine_name.o")
 done
 if [[ ${ENABLE_V8:-0} == 1 ]]; then
  engine_objects+=("$v8/out/musl/obj/libv8_monolith.a")
 fi
 if [[ ${ENABLE_SKIA:-0} == 1 ]]; then
  engine_objects+=("$root/work/chromium-build/skia/out/musl/libskia.a")
 fi
 engine_objects+=("$v8/out/musl/obj/buildtools/third_party/libc++/libc++.a" "$v8/out/musl/obj/buildtools/third_party/libc++abi/libc++abi.a")
fi
for name in allocator startup threads syscalls files shared probe; do
 clang "${common[@]}" "${includes[@]}" "${engine_flags[@]}" -c "$project/src/musl/$name.c" -o "$build/$name.o"
done
clang "${common[@]}" "${includes[@]}" -DUSE_MUSL -c "$project/src/posix.c" -o "$build/posix.o"
clang "${common[@]}" "${includes[@]}" -c "$project/src/virtual_memory.c" -o "$build/virtual_memory.o"
clang "${common[@]}" "${includes[@]}" -mgeneral-regs-only -c "$project/src/process/process.c" -o "$build/process.o"
clang "${common[@]}" "${includes[@]}" -c "$project/src/process/renderer_runtime.c" -o "$build/renderer_runtime.o"
clang "${common[@]}" "${includes[@]}" "${engine_flags[@]}" -c "$project/src/process/media_service.c" -o "$build/media_service.o"
engine_objects+=("$build/media_service.o")
clang "${common[@]}" "${includes[@]}" -c "$project/src/network/sockets.c" -o "$build/sockets.o"
clang "${common[@]}" "${includes[@]}" -c "$project/src/network/dns.c" -o "$build/dns.o"
clang "${common[@]}" "${includes[@]}" -c "$project/src/audio/ac97.c" -o "$build/audio.o"
clang -c "$project/src/process/entry.S" -o "$build/process-entry.o"
clang "${common[@]}" -c "$project/src/musl/thread-switch.S" -o "$build/thread-switch.o"
ld.lld --error-limit=0 --gc-sections -T "$project/src/payload.ld" -o "$build/payload.next.elf" "$build/"{allocator,startup,threads,thread-switch,syscalls,probe,posix,virtual_memory,process,process-entry,renderer_runtime,sockets,files,shared,dns,audio}.o "${engine_objects[@]}" "$libc_build/lib/libc.a" "$(clang -print-libgcc-file-name --rtlib=compiler-rt)"
mv -- "$build/payload.next.elf" "$build/payload.elf"
cat > "$build/embed.S" <<ASM
.section .rdata,"dr"
.balign 16
.globl payload_start
payload_start:
.incbin "$build/payload.elf"
.globl payload_end
payload_end:
ASM
loader_flags=()
loader_objects=()
if [[ $media_service == 1 ]]; then
 loader_flags+=(-DENABLE_MEDIA_SERVICE)
fi
if [[ $renderer_cpl3 == 1 ]]; then
 loader_flags+=(-DENABLE_RENDERER_CPL3 -mgeneral-regs-only)
fi
if [[ ${ENABLE_CPL3_PROBE:-0} == 1 ]]; then
 loader_flags+=(-DENABLE_CPL3_PROBE)
fi
if [[ ${ENABLE_BLINK:-0} == 1 ]]; then
 "$project/build-input-driver.sh"
 "$project/build-http-driver.sh"
 cat >> "$build/embed.S" <<ASM
.p2align 4
.global input_driver_start
.global input_driver_end
input_driver_start:
.incbin "$project/build/input-driver/VirtioInputDxe.efi"
input_driver_end:
.p2align 4
.global http_driver_start
.global http_driver_end
http_driver_start:
.incbin "$project/build/http-driver/HttpDxe.efi"
http_driver_end:
ASM
 loader_flags+=(-DENABLE_INPUT_DRIVER)
fi
clang --target=x86_64-pc-win32-coff -ffreestanding -fshort-wchar -mno-red-zone -fno-stack-protector -O2 -I"$project/runtime" "${loader_flags[@]}" -c "$project/src/loader.c" -o "$build/loader.obj"
clang --target=x86_64-pc-win32-coff -c "$build/embed.S" -o "$build/embed.obj"
clang --target=x86_64-pc-win32-coff -c "$project/runtime/platform/jump.S" -o "$build/jump.obj"
if [[ $media_service == 1 ]]; then
 clang --target=x86_64-pc-win32-coff -ffreestanding -fshort-wchar -mno-red-zone -fno-stack-protector -O2 -I"$project/runtime" "${loader_flags[@]}" -c "$project/src/loader-media.c" -o "$build/loader-media.obj"
 clang --target=x86_64-pc-win32-coff -include "$project/src/loader-media-symbols.h" -c "$project/runtime/platform/jump.S" -o "$build/media-jump.obj"
 loader_objects+=("$build/loader-media.obj" "$build/media-jump.obj")
fi
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 "/out:$build/esp/EFI/BOOT/BOOTX64.next.EFI" "$build/loader.obj" "$build/embed.obj" "$build/jump.obj" "${loader_objects[@]}"
mv -- "$build/esp/EFI/BOOT/BOOTX64.next.EFI" "$build/esp/EFI/BOOT/BOOTX64.EFI"
