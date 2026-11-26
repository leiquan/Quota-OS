#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
args=()
if [[ -n ${CLI_MODULE:-} ]]; then
    [[ $(wc -c < "$CLI_MODULE") -le 4096 ]] || exit 2
    args+=(-device "loader,file=$CLI_MODULE,addr=$(cat build/cli/module.addr),force-raw=on")
fi
if [[ -n ${CLI_DISK:-} ]]; then
    args+=(-global virtio-mmio.force-legacy=false -drive "if=none,id=notes,format=raw,file=$CLI_DISK" -device virtio-blk-device,drive=notes,serial=QUOTA-NOTES)
fi
exec qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a35 -m "${CLI_RAM:-32M}" -nic none -display none -monitor none -serial stdio -icount shift=4,sleep=off -kernel build/quotaos-cli.bin -append cli "${args[@]}"
