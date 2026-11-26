#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
if ! command -v qemu-system-aarch64 >/dev/null; then
    echo "qemu-system-aarch64 is required. Install QEMU with ARM system emulation and retry." >&2
    exit 127
fi
kernel=${QUOTA_KERNEL:-build/quotaos-lvgl.bin}
test -f "$kernel" || { echo "Build the account-enabled image with tools/build-lvgl-guest.sh: $kernel" >&2; exit 1; }
disk=${QUOTA_NOTES_DISK:-"${XDG_DATA_HOME:-$HOME/.local/share}/quota-os/notes.raw"}
mkdir -p "$(dirname "$disk")"
if [ ! -e "$disk" ]; then
    python3 tools/init-admin-disk.py "$disk"
fi
test "$(wc -c < "$disk")" -ge 32768 || { echo "Use tools/init-admin-disk.py NEW --from-legacy OLD to create an account-enabled copy" >&2; exit 1; }
exec /bin/bash tools/run-qemu-gui.sh -machine virt,gic-version=2 -cpu cortex-a35 -m 64M -nic none \
    -global virtio-mmio.force-legacy=false -device virtio-rng-device -device ramfb -device virtio-tablet-device \
    -device virtio-keyboard-device -drive "if=none,id=notes,format=raw,file=$disk" \
    -device virtio-blk-device,drive=notes,serial=QUOTA-NOTES -icount shift=4,sleep=off \
    -kernel "$kernel" -append apps-video -serial "${QUOTA_SERIAL:-stdio}" -monitor none "$@"
