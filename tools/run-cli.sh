#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
args=()
if [[ ${CLI_TEST:-0} == 1 ]]; then
    image=build/cli/test.bin
    size=$(wc -c < "$image")
    (( size > 0 && size <= 4096 )) || exit 2
    addr=$(cat build/cli/module.addr)
    args+=(-device "loader,file=$image,addr=$addr,force-raw=on"
           -device "loader,addr=$((addr+4096)),data=$((size)),data-len=4")
fi
if [[ -n ${CLI_DISK:-} ]]; then
    args+=(-global virtio-mmio.force-legacy=false -drive "if=none,id=notes,format=raw,file=$CLI_DISK" -device virtio-blk-device,drive=notes,serial=QUOTA-NOTES)
fi
exec qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a35 -m "${CLI_RAM:-16M}" -nic none -display none -monitor none -serial stdio -icount shift=4,sleep=off -kernel build/quotaos-cli.bin -append cli ${args[@]+"${args[@]}"}
