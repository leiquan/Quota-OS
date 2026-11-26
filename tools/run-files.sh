#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
if ! command -v qemu-system-aarch64 >/dev/null; then
    echo "qemu-system-aarch64 is required. Install QEMU with ARM system emulation and retry." >&2
    exit 127
fi
ram=${QUOTA_QEMU_RAM:-128M}
bootargs=lvgl
kernel=${QUOTA_KERNEL:-build/quotaos-lvgl.bin}
root_args=()
if [[ -n ${QUOTA_ROOT_DISK:-} ]]; then
    ram=${QUOTA_QEMU_RAM:-1G}
    bootargs=apps-ext4
    kernel=${QUOTA_KERNEL:-build/quotaos-node-web.bin}
    test -f "$QUOTA_ROOT_DISK" || { echo "Root disk not found: $QUOTA_ROOT_DISK" >&2; exit 1; }
    root_args=(-drive "if=none,id=root,format=raw,cache=writeback,werror=report,rerror=report,file=${QUOTA_ROOT_DISK//,/,,}"
               -device virtio-blk-device,bus=virtio-mmio-bus.30,drive=root,serial=QUOTA-ROOT)
fi
test -f "$kernel" || { echo "Kernel image not found: $kernel" >&2; exit 1; }
disk=${QUOTA_NOTES_DISK:-"${XDG_DATA_HOME:-$HOME/.local/share}/quota-os/touch-ui-account.raw"}
mkdir -p "$(dirname "$disk")"
if [ ! -e "$disk" ]; then
    python3 tools/init-admin-disk.py "$disk"
fi
test "$(wc -c < "$disk")" -ge 32768 || { echo "Use tools/init-admin-disk.py NEW --from-legacy OLD to create an account-enabled copy" >&2; exit 1; }
if [[ -n ${QUOTA_ROOT_DISK:-} && "$QUOTA_ROOT_DISK" -ef "$disk" ]]; then
    echo "Root and Notes must use different disk images." >&2
    exit 1
fi
exec /bin/bash tools/run-qemu-gui.sh -machine virt,gic-version=2 -cpu cortex-a35 -m "$ram" -nic none \
    -global virtio-mmio.force-legacy=false ${root_args[@]+"${root_args[@]}"} -drive "if=none,id=notes,format=raw,file=${disk//,/,,}" \
    -device virtio-blk-device,bus=virtio-mmio-bus.29,drive=notes,serial=QUOTA-NOTES \
    -device ramfb -device virtio-tablet-device -device virtio-rng-device -device virtio-keyboard-device \
    -icount shift=4,sleep=off -kernel "$kernel" -append "$bootargs" -serial stdio -monitor none "$@"
