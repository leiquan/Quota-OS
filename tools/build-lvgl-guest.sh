#!/bin/bash
# LVGL guest. Upstream tree: third_party/lvgl (v9.3.0, see PIN.txt).
# Writes build/quotaos-lvgl.bin. After a QEMU check, that file is installed
# as prebuilt/quotaos.bin and copied to prebuilt/quotaos-lvgl.bin.
# Does not touch prebuilt/quotaos-serial.bin or history.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
source tools/guest-toolchain.sh
EXTRA_CFLAGS=${EXTRA_CFLAGS:-}
image_name=quotaos-lvgl
if [[ -n "${QUOTA_NODE_WEB_OUT:-}${QUOTA_WAP_PAGE_ELF:-}${QUOTA_WAP_INTERPRETER:-}" ]]; then
    source tools/wap-runtime.sh
    configure_wap_runtime
fi
CFLAGS=($EXTRA_CFLAGS -std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding -fno-stack-protector
    -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
    -fno-asynchronous-unwind-tables -I include -I build)
mkdir -p build build/user
short=$(git rev-parse --short=7 HEAD)
printf '#define QUOTAOS_COMMIT "%s"\n' "$short" > build/commit.h
echo "banner parent $short"
"${cc[@]}" "${link_flags[@]}" -Wl,--build-id=none \
    -T user/user.ld -o build/user/paint.elf user/paint.S
$OBJCOPY -O binary build/user/paint.elf build/user/paint.bin
objs=()
while IFS= read -r f; do
    obj="build/${f%.*}.o"
    mkdir -p "$(dirname "$obj")"
    case "$f" in
        *.S) "${cc[@]}" $EXTRA_CFLAGS -Iinclude -Ibuild -c -o "$obj" "$f" ;;
        *) "${cc[@]}" "${CFLAGS[@]}" -c -o "$obj" "$f" ;;
    esac
    objs+=("$obj")
done < <(cat kernel/sources.list port/quotaos/wap/sources.list)
LVFLAGS=(${lv_headers[@]:+"${lv_headers[@]}"} $EXTRA_CFLAGS -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-builtin
    -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
    -fno-asynchronous-unwind-tables -Wno-unused-parameter -w
    -DLV_CONF_INCLUDE_SIMPLE -I third_party/lvgl -I port/quotaos -I include)
mkdir -p build/lvgl build/port
while IFS= read -r src; do
    rel=${src#third_party/lvgl/}
    obj="build/lvgl/${rel%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "${cc[@]}" "${LVFLAGS[@]}" -c -o "$obj" "$src"
    objs+=("$obj")
done < <(find third_party/lvgl/src/libs/bin_decoder -name '*.c'; find third_party/lvgl/src \
    -path 'third_party/lvgl/src/libs' -prune -o \
    -path 'third_party/lvgl/src/drivers' -prune -o \
    -path 'third_party/lvgl/src/draw/vg_lite' -prune -o \
    -path 'third_party/lvgl/src/draw/nema_gfx' -prune -o \
    -path 'third_party/lvgl/src/draw/nxp' -prune -o \
    -path 'third_party/lvgl/src/draw/sdl' -prune -o \
    -path 'third_party/lvgl/src/draw/opengles' -prune -o \
    -path 'third_party/lvgl/src/draw/dma2d' -prune -o \
    -path 'third_party/lvgl/src/draw/renesas' -prune -o \
    -name '*.c' -print | sort)
for src in port/quotaos/desktop/*.c; do
    obj="build/${src%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "${cc[@]}" "${LVFLAGS[@]}" -c -o "$obj" "$src"
    objs+=("$obj")
done
if [[ -n "${QUOTA_WAP_PAGE_ELF:-}" ]]; then
    embed_wap_runtime
fi
"${cc[@]}" "${link_flags[@]}" -Wl,--build-id=none \
    -T boot/linker.ld -o "build/$image_name.elf" "${objs[@]}" $RUNTIME_LIB
$OBJCOPY -O binary "build/$image_name.elf" "build/$image_name.bin"
echo "linked $(wc -c < "build/$image_name.bin") bytes"
$NM "build/$image_name.elf" | grep -E "lvgl_demo_run|__image_end" || true
