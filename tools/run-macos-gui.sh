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
exec qemu-system-aarch64 -accel hvf -machine virt,gic-version=3 \
    -cpu host -m 128M -display cocoa -nic none -device ramfb \
    -device virtio-keyboard-device -device virtio-tablet-device \
    -kernel "$kernel" -serial stdio -monitor none
