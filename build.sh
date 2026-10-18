#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd "$project/../.." && pwd)
skia="$root/work/chromium-build/skia"
mkdir -p "$project/build" "$project/build/esp/EFI/BOOT"
browser="$project/runtime"
common=(-O2 -fno-builtin -fno-stack-protector -mno-red-zone -fno-pic -fno-pie -ffunction-sections -fdata-sections)
for source in compat; do
 clang "${common[@]}" -ffreestanding -I"$browser/platform/include" -I"$browser" -c "$browser/platform/$source.c" -o "$project/build/$source.o"
done
for source in runtime platform math_hyperbolic; do
 clang "${common[@]}" -ffreestanding -I"$browser/platform/include" -I"$browser" -c "$project/src/$source.c" -o "$project/build/$source.o"
done
engine_objects=()
engine_flags=()
if [[ ${ENABLE_V8:-0} == 1 ]]; then
 engine_flags=(-DENABLE_V8)
 clang++ "${common[@]}" -std=c++20 -fno-exceptions -fno-rtti -I"$root/work/chromium-build/v8/include" -I"$root/work/chromium-build/v8/out/uefi/gen/include" -c "$project/src/v8_probe.cpp" -o "$project/build/v8_probe.o"
 engine_objects=("$project/build/v8_probe.o" "$root/work/chromium-build/v8/out/uefi/obj/libv8_monolith.a")
fi
for source in probe cxx; do
 clang++ "${common[@]}" "${engine_flags[@]}" -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -DNDEBUG -I"$skia" -I"$browser" -c "$project/src/$source.cpp" -o "$project/build/$source.o"
done
clang++ "${common[@]}" -std=c++20 -fno-exceptions -fno-rtti -DLIBC_NAMESPACE=__llvm_libc_uefi -I"$root/work/chromium-build/v8/third_party/llvm-libc/src" -c "$project/src/math_exact.cpp" -o "$project/build/math_exact.o"
clang "${common[@]}" -I"$browser" -c "$project/src/posix.c" -o "$project/build/posix.o"
ld.lld --error-limit=0 --gc-sections -T "$project/src/payload.ld" -o "$project/build/payload.next.elf" \
 "$project/build/"{probe,cxx,runtime,math_hyperbolic,math_exact,compat,platform,posix}.o "$skia/out/uefi/libskia.a" "${engine_objects[@]}" "$(clang++ -print-file-name=libstdc++.a)"
mv -- "$project/build/payload.next.elf" "$project/build/payload.elf"
cat > "$project/build/embed.S" <<ASM
.section .rdata,"dr"
.balign 16
.globl payload_start
payload_start:
.incbin "$project/build/payload.elf"
.globl payload_end
payload_end:
ASM
clang --target=x86_64-pc-win32-coff -ffreestanding -fshort-wchar -mno-red-zone -fno-stack-protector -O2 -I"$browser" -c "$project/src/loader.c" -o "$project/build/loader.obj"
clang --target=x86_64-pc-win32-coff -c "$project/build/embed.S" -o "$project/build/embed.obj"
clang --target=x86_64-pc-win32-coff -c "$browser/platform/jump.S" -o "$project/build/jump.obj"
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 "/out:$project/build/esp/EFI/BOOT/BOOTX64.next.EFI" "$project/build/loader.obj" "$project/build/embed.obj" "$project/build/jump.obj"

mv -- "$project/build/esp/EFI/BOOT/BOOTX64.next.EFI" "$project/build/esp/EFI/BOOT/BOOTX64.EFI"
