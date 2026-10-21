#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
build=${BUILD_DIRECTORY:-"$project/build/virtio-gpu-probe"}
mkdir -p "$build/esp/EFI/BOOT"
extra=()
if [[ ${ENABLE_VENUS_COMMANDS:-0} == 1 ]]; then
  source_directory=$(python3 "$project/prepare-venus.py")
  extra=(-DENABLE_VENUS_COMMANDS=1 -I"$project/src/gpu/venus"
    -isystem "$source_directory/src/virtio/virtio-gpu"
    -isystem "$source_directory/src/virtio/venus-protocol" -isystem "$source_directory/include"
    -I"$project/runtime/platform/include" )
fi
if [[ ${ENABLE_VENUS_RENDER:-0} == 1 ]]; then
  if [[ ${ENABLE_VENUS_COMMANDS:-0} != 1 ]]; then
    echo "ENABLE_VENUS_RENDER requires ENABLE_VENUS_COMMANDS=1" >&2
    exit 1
  fi
  python3 "$project/tools/build-venus-shaders.py" "$build/shaders"
  extra+=(-DENABLE_VENUS_RENDER=1 -I"$build/shaders")
fi
clang "${extra[@]}" --target=x86_64-pc-win32-coff -ffreestanding -fno-builtin -fshort-wchar \
  -mno-red-zone -fno-stack-protector -O2 -Wall -Wextra -I"$project/runtime" \
  -c "$project/src/gpu/virtio-probe.c" -o "$build/virtio-probe.obj"
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 \
  "/out:$build/esp/EFI/BOOT/BOOTX64.EFI" "$build/virtio-probe.obj"
