#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
args=()
bootargs=cli
if [[ $# == 1 && $1 == --quiet ]]; then
    args+=(-device "loader,addr=$(cat build/cli/quiet.addr),data=1,data-len=1")
elif [[ $# != 0 ]]; then
    echo "Usage: bash tools/run-cli.sh [--quiet]" >&2
    exit 2
fi
if [[ ${CLI_TEST:-1} == 1 ]]; then
    image=build/cli/test.bin
    size=$(wc -c < "$image")
    (( size > 0 && size <= 4096 )) || exit 2
    source_file=build/cli/test.c
    source_size=$(wc -c < "$source_file")
    (( source_size > 0 && source_size < 4096 )) || exit 2
    addr=$(cat build/cli/module.addr)
    args+=(-device "loader,file=$source_file,addr=$((addr+4100)),force-raw=on"
           -device "loader,file=$image,addr=$addr,force-raw=on"
           -device "loader,addr=$((addr+4096)),data=$((size)),data-len=4")
fi
if [[ ${CLI_ROOT_RW:-0} == 1 ]]; then
    if [[ -z ${CLI_ROOT_DISK:-} || ( -n ${CLI_DISK:-} && "$CLI_ROOT_DISK" -ef "$CLI_DISK" ) ]]; then
        echo "CLI_ROOT_RW=1 requires a dedicated CLI_ROOT_DISK" >&2
        exit 2
    fi
    bootargs="cli ext4rw"
    if [[ ${CLI_ROOT_CRASH_TEST:-0} == 1 ]]; then bootargs="cli ext4rw crash-test"; fi
    args+=(-global virtio-mmio.force-legacy=false
           -drive "if=none,id=root,format=raw,cache=writeback,werror=report,rerror=report,file=$CLI_ROOT_DISK"
           -device virtio-blk-device,bus=virtio-mmio-bus.30,drive=root,serial=QUOTA-ROOT)
elif [[ -n ${CLI_ROOT_DISK:-} ]]; then
    args+=(-global virtio-mmio.force-legacy=false
           -drive "if=none,id=root,format=raw,readonly=on,file=$CLI_ROOT_DISK"
           -device virtio-blk-device,drive=root,serial=QUOTA-ROOT)
fi
if [[ -n ${CLI_DISK:-} ]]; then
    args+=(-global virtio-mmio.force-legacy=false -drive "if=none,id=notes,format=raw,file=$CLI_DISK" -device virtio-blk-device,drive=notes,serial=QUOTA-NOTES)
fi
if [[ -t 1 ]]; then printf '\033[2J\033[H'; fi
exec qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a35 -m "${CLI_RAM:-4M}" -nic none -display none -monitor none -serial stdio -icount shift=4,sleep=off -kernel build/quotaos-cli.bin -append "$bootargs" ${args[@]+"${args[@]}"}
