#!/bin/bash
# Boot the external-runtime development image with a separate Notes disk.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "Usage: $0 READ_ONLY_RUNTIME_VOLUME NOTES_DISK" >&2
    exit 2
fi
runtime=$1
notes=$2
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
kernel=${QUOTA_KERNEL:-"$root/build/quotaos-node-web-volume.bin"}
test -r "$kernel"
test -r "$runtime"
if [[ ! -e "$notes" ]]; then
    (set -C; umask 077; dd if=/dev/zero bs=1024 count=64 >"$notes")
fi
exec qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a35 -m 512M \
    -nic none -global virtio-mmio.force-legacy=false -device virtio-rng-device \
    -device ramfb -device virtio-tablet-device -device virtio-keyboard-device \
    -drive "if=none,id=notes,format=raw,file=$notes" \
    -device virtio-blk-device,drive=notes,serial=QUOTA-NOTES \
    -drive "if=none,id=runtime,format=raw,readonly=on,file=$runtime" \
    -device virtio-blk-device,drive=runtime,serial=QUOTA-RUNTIME \
    -icount shift=1,sleep=off -kernel "$kernel" -append apps-video \
    -serial stdio -monitor none -display "${QUOTA_DISPLAY:-default}"
