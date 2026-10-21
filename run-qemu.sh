#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
default_build="$project/build/blink"
build=${BUILD_DIRECTORY:-"$default_build"}
default_memory=1024
if [[ "$build" == "$project/build/blink" ]]; then
 default_memory=4096
fi
if [[ ! -f "$build/esp/EFI/BOOT/BOOTX64.EFI" ]]; then
 if [[ "$build" != "$default_build" ]]; then
  printf '%s\n' "Missing image in $build; build the selected runtime first." >&2
  exit 1
 fi
 "$project/build-blink.sh"
fi
variables=${QEMU_VARIABLES:-"$build/UEFI_BROWSER_VARS.fd"}
# The firmware profile retains browser preferences across launches.
exec 9>"$variables.lock"
flock -n 9 || { printf '%s\n' 'This firmware profile is already in use.' >&2; exit 1; }
if [[ ! -f "$variables" ]]; then
 cp /usr/share/OVMF/OVMF_VARS_4M.fd "$variables"
fi
display_arguments=(-display "${QEMU_DISPLAY:-sdl}")
if [[ -n ${QEMU_VNC:-} ]]; then
 display_arguments+=(-vnc "$QEMU_VNC")
 printf '%s\n' "VNC display: $QEMU_VNC"
fi
network_arguments=(-netdev user,id=network -device virtio-net-pci,netdev=network,romfile=)
if [[ -f "$project/build/ca.esl" ]]; then
 network_arguments+=(-fw_cfg "name=etc/edk2/https/cacerts,file=$project/build/ca.esl")
fi
case "${QEMU_ACCEL:-auto}" in
 auto) acceleration_arguments=(-accel kvm -accel tcg) ;;
 kvm|tcg) acceleration_arguments=(-accel "$QEMU_ACCEL") ;;
 *) printf '%s\n' 'QEMU_ACCEL must be auto, kvm or tcg.' >&2; exit 1 ;;
esac
disk_arguments=(-drive "format=raw,file=fat:rw:$build/esp")
machine_arguments=(-machine q35)
gpu_arguments=()
binary_arguments=()
qemu_binary=${QEMU_BINARY:-qemu-system-x86_64}
if [[ ${QEMU_GPU_COMPOSITION:-0} == 1 ]]; then
 if [[ ${QEMU_ACCEL:-kvm} != kvm ]]; then
  printf '%s\n' 'GPU composition requires QEMU_ACCEL=kvm.' >&2
  exit 1
 fi
 qemu_binary=${QEMU_BINARY:-"$project/build/qemu-scanout/qemu-system-x86_64"}
 default_memory=4096
 binary_arguments=(-L /usr/share/qemu)
 machine_arguments=(-machine q35,memory-backend=memory)
 acceleration_arguments=(-accel kvm,honor-guest-pat=on)
 gpu_arguments=(-vga none -object "memory-backend-memfd,id=memory,size=${QEMU_MEMORY:-4096}M,share=on"
  -device virtio-vga-gl,blob=on,venus=on,hostmem=512M,uefi-scanout-modifier=on,romfile=/usr/share/seabios/vgabios-virtio.bin)
 display_arguments=(-display "${QEMU_DISPLAY:-sdl,gl=on}")
 export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-x11}
 export SDL_VIDEO_X11_FORCE_EGL=1
fi
if [[ -n ${QEMU_DISK_IMAGE:-} ]]; then
 if [[ ! -f "$QEMU_DISK_IMAGE" ]]; then
  printf '%s\n' "Missing disk image: $QEMU_DISK_IMAGE" >&2
  exit 1
 fi
 disk_arguments=(-drive "format=raw,file=$QEMU_DISK_IMAGE")
fi
"$qemu_binary" "${binary_arguments[@]}" -name "UEFI BROWSER" "${machine_arguments[@]}" "${acceleration_arguments[@]}" "${gpu_arguments[@]}" -m "${QEMU_MEMORY:-$default_memory}" -smp "${QEMU_CPUS:-4}" \
 -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
 -drive "if=pflash,format=raw,file=$variables" \
 "${disk_arguments[@]}" \
 -device virtio-mouse-pci -device virtio-rng-pci "${network_arguments[@]}" "${display_arguments[@]}" \
 -audiodev "${QEMU_AUDIO_DRIVER:-pipewire},id=audio" -device AC97,audiodev=audio \
 -serial stdio -no-reboot
