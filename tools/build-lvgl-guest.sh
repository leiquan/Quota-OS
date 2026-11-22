#!/bin/bash
# LVGL guest. Upstream tree: third_party/lvgl (v9.3.0, see PIN.txt).
# Writes build/quotaos-lvgl.bin. After a QEMU check, that file is installed
# as prebuilt/quotaos.bin and copied to prebuilt/quotaos-lvgl.bin.
# Does not touch prebuilt/quotaos-serial.bin or history.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
CC=${CC:-aarch64-linux-gnu-gcc}
OBJCOPY=${OBJCOPY:-aarch64-linux-gnu-objcopy}
NM=${NM:-aarch64-linux-gnu-nm}
RUNTIME_LIB=${RUNTIME_LIB:--lgcc}
EXTRA_CFLAGS=${EXTRA_CFLAGS:-}
CFLAGS=($EXTRA_CFLAGS -std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding -fno-stack-protector
    -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
    -fno-asynchronous-unwind-tables -I include -I build)
mkdir -p build build/user
short=$(git rev-parse --short=7 HEAD)
printf '#define QUOTAOS_COMMIT "%s"\n' "$short" > build/commit.h
echo "banner parent $short"
$CC -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none \
    -T user/user.ld -o build/user/paint.elf user/paint.S
$OBJCOPY -O binary build/user/paint.elf build/user/paint.bin
objs=()
while IFS= read -r f; do
    obj="build/${f%.*}.o"
    mkdir -p "$(dirname "$obj")"
    $CC "${CFLAGS[@]}" -c -o "$obj" "$f"
    objs+=("$obj")
done < kernel/sources.list
LVFLAGS=($EXTRA_CFLAGS -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-builtin
    -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
    -fno-asynchronous-unwind-tables -Wno-unused-parameter -w
    -DLV_CONF_INCLUDE_SIMPLE -I third_party/lvgl -I port/quotaos -I include)
mkdir -p build/lvgl build/port
while IFS= read -r src; do
    rel=${src#third_party/lvgl/}
    obj="build/lvgl/${rel%.c}.o"
    mkdir -p "$(dirname "$obj")"
    $CC "${LVFLAGS[@]}" -c -o "$obj" "$src"
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
    $CC "${LVFLAGS[@]}" -c -o "$obj" "$src"
    objs+=("$obj")
done
$CC -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none \
    -T boot/linker.ld -o build/quotaos-lvgl.elf "${objs[@]}" $RUNTIME_LIB
$OBJCOPY -O binary build/quotaos-lvgl.elf build/quotaos-lvgl.bin
echo "linked $(wc -c < build/quotaos-lvgl.bin) bytes"
$NM build/quotaos-lvgl.elf | grep -E "lvgl_demo_run|__image_end" || true
