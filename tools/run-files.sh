#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
if ! command -v qemu-system-aarch64 >/dev/null; then
    echo "qemu-system-aarch64 is required. Install QEMU with ARM system emulation and retry." >&2
    exit 127
fi
kernel=${QUOTA_KERNEL:-prebuilt/quotaos.bin}
test -f "$kernel" || { echo "Kernel image not found: $kernel" >&2; exit 1; }
disk=${QUOTA_NOTES_DISK:-"${XDG_DATA_HOME:-$HOME/.local/share}/quota-os/notes.raw"}
mkdir -p "$(dirname "$disk")"
if [ ! -e "$disk" ]; then
    (set -C; umask 077; dd if=/dev/zero bs=1024 count=8 > "$disk")
fi
test "$(wc -c < "$disk")" -eq 8192 || { echo "Expected an 8192-byte dedicated Notes image" >&2; exit 1; }
exec qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a35 -m 64M -nic none \
    -global virtio-mmio.force-legacy=false -device ramfb -device virtio-tablet-device \
    -device virtio-keyboard-device -drive "if=none,id=notes,format=raw,file=$disk" \
    -device virtio-blk-device,drive=notes,serial=QUOTA-NOTES -icount shift=4,sleep=off \
    -kernel "$kernel" -append apps-video -serial stdio -monitor none "$@"
