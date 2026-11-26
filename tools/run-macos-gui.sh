#!/bin/bash
# Apple Silicon: real host time with native CPU execution, not icount.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
    echo 'This launcher requires an Apple Silicon Mac.' >&2
    exit 1
fi
kernel=${1:-build/quotaos-lvgl.bin}
if [[ ! -f "$kernel" ]]; then
    echo 'Build the image first: bash tools/build-lvgl-macos.sh' >&2
    exit 1
fi
disk=${QUOTA_NOTES_DISK:-"${XDG_DATA_HOME:-$HOME/.local/share}/quota-os/touch-ui-account.raw"}
mkdir -p "$(dirname "$disk")"
if [[ ! -e "$disk" ]]; then
    python3 tools/init-admin-disk.py "$disk"
fi
test "$(wc -c < "$disk")" -ge 32768 || { echo "Use tools/init-admin-disk.py NEW --from-legacy OLD to create an account-enabled copy" >&2; exit 1; }

exec /bin/bash tools/run-qemu-gui.sh -accel hvf -machine virt,gic-version=3 \
    -cpu host -m "${QUOTA_QEMU_RAM:-128M}" -display cocoa,full-screen=off,zoom-to-fit=off,show-cursor=off \
    -nic none -device ramfb -device virtio-rng-device -global virtio-mmio.force-legacy=false \
    -drive "if=none,id=notes,format=raw,file=$disk" -device virtio-blk-device,drive=notes,serial=QUOTA-NOTES \
    -device virtio-keyboard-device -device virtio-tablet-device \
    -kernel "$kernel" -serial stdio -monitor none
