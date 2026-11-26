#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
kernel="$root/prebuilt/quotaos-jsc.bin"
fw=()
if [[ ${1:-} == --page || ${1:-} == --page-bytes ]]; then
    mode=$1
    [[ -f ${2:-} && -r $2 ]] || { echo "Usage: $0 --page FILE.html [QEMU options]" >&2; exit 2; }
    page=$2
    if [[ $mode == --page ]]; then
        [[ $(uname -s) == Darwin ]] || { echo "Static WebKit preview requires macOS. Use --page-bytes for the guest loader test." >&2; exit 2; }
        temp=$(mktemp -d)
        trap 'rm -rf -- "$temp"' EXIT
        size=$(wc -c < "$page")
        (( size > 0 && size <= 16384 )) || { echo "Expected 1..16384 bytes of UTF-8 HTML" >&2; exit 2; }
        cp "$page" "$temp/page.html"
        page="$temp/page.html"
        xcrun clang -fobjc-arc -framework Cocoa -framework WebKit "$root/tools/macos/webkit-snapshot.m" -o "$temp/render"
        "$temp/render" "$page" "$temp/frame"
    fi
    fw=(-fw_cfg "name=opt/quota/document,file=${page//,/,,}")
    kernel="$root/prebuilt/quotaos-webkit-loader.bin"
    if [[ $mode == --page ]]; then fw+=(-fw_cfg "name=opt/quota/webkit-frame,file=${temp//,/,,}/frame"); fi
    shift 2
fi
set -- /bin/bash "$root/tools/run-qemu-gui.sh" -machine virt,gic-version=2 -cpu cortex-a72 -m 128M \
    -nic none -device ramfb -icount shift=4,sleep=off ${fw[@]+"${fw[@]}"} \
    -kernel "$kernel" -append webkit -serial stdio -monitor none "$@"

if [[ ${mode:-} == --page ]]; then "$@"; else exec "$@"; fi
