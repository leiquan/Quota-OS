#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
kernel="$root/prebuilt/quotaos-jsc.bin"
fw=()
if [[ ${1:-} == --page ]]; then
    [[ -f ${2:-} && -r $2 ]] || { echo "Usage: $0 --page FILE.html [QEMU options]" >&2; exit 2; }
    page=$2
    fw=(-fw_cfg "name=opt/quota/document,file=${page//,/,,}")
    kernel="$root/prebuilt/quotaos-webkit-loader.bin"
    shift 2
fi
exec qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 128M \
    -nic none -device ramfb -icount shift=4,sleep=off "${fw[@]}" \
    -kernel "$kernel" -append webkit -serial stdio -monitor none "$@"
