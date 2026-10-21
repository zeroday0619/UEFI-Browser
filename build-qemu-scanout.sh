#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
source_directory=${QEMU_SOURCE:?Set QEMU_SOURCE to an unpacked QEMU 11.1.2 source tree.}
source_directory=$(cd -- "$source_directory" && pwd)
build_directory=${QEMU_BUILD_DIRECTORY:-"$project/build/qemu-scanout"}
patch_file="$project/patches/qemu-uefi-scanout-modifier.patch"
if [[ $(cat "$source_directory/VERSION") != 11.1.2 ]]; then
 printf '%s\n' 'This patch requires QEMU 11.1.2.' >&2
 exit 1
fi
# Repeated builds preserve an already applied patch and reject unrelated source drift.
if patch --dry-run --batch --forward -p1 -d "$source_directory" < "$patch_file" >/dev/null 2>&1; then
 patch --batch --forward -p1 -d "$source_directory" < "$patch_file"
elif ! patch --dry-run --batch --reverse -p1 -d "$source_directory" < "$patch_file" >/dev/null 2>&1; then
 printf '%s\n' 'QEMU source does not match the scanout patch.' >&2
 exit 1
fi
mkdir -p "$build_directory"
cd -- "$build_directory"
"$source_directory/configure" --python="${QEMU_BUILD_PYTHON:-/usr/bin/python3}" \
 --target-list=x86_64-softmmu --disable-docs --disable-werror --disable-rust \
 --disable-tools --disable-guest-agent --disable-fdt --disable-install-blobs \
 --enable-opengl --enable-sdl --enable-virglrenderer --enable-slirp
ninja -j "${JOBS:-4}" qemu-system-x86_64
printf 'Built patched QEMU: %s/qemu-system-x86_64\n' "$PWD"
