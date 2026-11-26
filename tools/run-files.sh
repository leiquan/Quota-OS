#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
if ! command -v qemu-system-aarch64 >/dev/null; then
    echo "qemu-system-aarch64 is required. Install QEMU with ARM system emulation and retry." >&2
    exit 127
fi

ram=${QUOTA_QEMU_RAM:-512M}
cpus=${QUOTA_TOUCH_CPUS:-2}
case "$cpus" in 1|2) ;; *) echo "Touch UI supports one or two configured CPUs." >&2; exit 2 ;; esac
bootargs=lvgl
default_kernel=build/quotaos-lvgl.bin
cpu_args=(-machine virt,gic-version=2 -cpu cortex-a35 -icount shift=1,sleep=off)
if [[ ${QUOTA_TOUCH_HVF:-0} == 1 ]]; then
    cpu_args=(-accel hvf -machine virt,gic-version=3 -cpu host)
fi
disks=()
if [[ -n ${QUOTA_RUNTIME_VOLUME:-} ]]; then
    runtime=$QUOTA_RUNTIME_VOLUME
    test -r "$runtime" || { echo "Missing read-only runtime volume: $runtime" >&2; exit 1; }
    bootargs=apps-video
    default_kernel=build/quotaos-node-web-volume.bin
    disks+=(-drive "if=none,id=runtime,format=raw,readonly=on,file=${runtime//,/,,}"
            -device virtio-blk-device,bus=virtio-mmio-bus.28,drive=runtime,serial=QUOTA-RUNTIME)
fi
kernel=${QUOTA_KERNEL:-$default_kernel}
test -f "$kernel" || { echo "Kernel image not found: $kernel" >&2; exit 1; }
disk=${QUOTA_NOTES_DISK:-"${XDG_DATA_HOME:-$HOME/.local/share}/quota-os/touch-ui-account.raw"}
mkdir -p "$(dirname "$disk")"
if [ ! -e "$disk" ]; then
    python3 tools/init-admin-disk.py "$disk"
fi
test "$(wc -c < "$disk")" -ge 32768 || { echo "Use tools/init-admin-disk.py NEW --from-legacy OLD to create an account-enabled copy" >&2; exit 1; }
if [[ -n ${runtime:-} && "$runtime" -ef "$disk" ]]; then
    echo "Notes must use a separate disk image." >&2
    exit 1
fi
source tools/root-volume.sh
if [[ -n ${root_disk:-} ]]; then
    bootargs=apps-ext4
    disks+=("${root_args[@]}")
fi

exec /bin/bash tools/run-qemu-gui.sh "${cpu_args[@]}" -m "$ram" -smp "$cpus" -nic none \
    -global virtio-mmio.force-legacy=false ${disks[@]+"${disks[@]}"} \
    -drive "if=none,id=notes,format=raw,file=${disk//,/,,}" \
    -device virtio-blk-device,bus=virtio-mmio-bus.29,drive=notes,serial=QUOTA-NOTES \
    -device ramfb -device virtio-tablet-device -device virtio-rng-device -device virtio-keyboard-device \
    -kernel "$kernel" -append "$bootargs" -serial "${QUOTA_SERIAL:-stdio}" -monitor none "$@"
