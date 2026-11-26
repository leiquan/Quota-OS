# Shared image build. The edition entry point supplies source_lists, image_name and an optional payload hook.
CFLAGS=($EXTRA_CFLAGS -std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding -fno-stack-protector
    -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
    -fno-asynchronous-unwind-tables -I os/include -I build)
mkdir -p build build/user
short=$(git rev-parse --short=7 HEAD)
printf '#define QUOTAOS_COMMIT "%s"\n' "$short" > build/commit.h
echo "banner parent $short"
"${cc[@]}" "${link_flags[@]}" -Wl,--build-id=none \
    -T os/user/user.ld -o build/user/paint.elf os/user/paint.S
$OBJCOPY -O binary build/user/paint.elf build/user/paint.bin
objs=()
while IFS= read -r f; do
    obj="build/${f%.*}.o"
    mkdir -p "$(dirname "$obj")"
    case "$f" in
        *.S) "${cc[@]}" $EXTRA_CFLAGS -Ios/include -Ibuild -c -o "$obj" "os/$f" ;;
        *) "${cc[@]}" "${CFLAGS[@]}" -c -o "$obj" "os/$f" ;;
    esac
    objs+=("$obj")
done < <(cat "${source_lists[@]}")
LVFLAGS=(${lv_headers[@]:+"${lv_headers[@]}"} $EXTRA_CFLAGS -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-builtin
    -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
    -fno-asynchronous-unwind-tables -Wno-unused-parameter -w
    -DLV_CONF_INCLUDE_SIMPLE -I os/third_party/lvgl -I os/port -I os/include)
mkdir -p build/lvgl build/port
while IFS= read -r src; do
    rel=${src#os/third_party/lvgl/}
    obj="build/lvgl/${rel%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "${cc[@]}" "${LVFLAGS[@]}" -c -o "$obj" "$src"
    objs+=("$obj")
done < <(find os/third_party/lvgl/src/libs/bin_decoder -name '*.c'; find os/third_party/lvgl/src \
    -path 'os/third_party/lvgl/src/libs' -prune -o \
    -path 'os/third_party/lvgl/src/drivers' -prune -o \
    -path 'os/third_party/lvgl/src/draw/vg_lite' -prune -o \
    -path 'os/third_party/lvgl/src/draw/nema_gfx' -prune -o \
    -path 'os/third_party/lvgl/src/draw/nxp' -prune -o \
    -path 'os/third_party/lvgl/src/draw/sdl' -prune -o \
    -path 'os/third_party/lvgl/src/draw/opengles' -prune -o \
    -path 'os/third_party/lvgl/src/draw/dma2d' -prune -o \
    -path 'os/third_party/lvgl/src/draw/renesas' -prune -o \
    -name '*.c' -print | sort)
for src in os/port/desktop/*.c; do
    rel=${src#os/}
    obj="build/${rel%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "${cc[@]}" "${LVFLAGS[@]}" -c -o "$obj" "$src"
    objs+=("$obj")
done
if [[ -n "${embed_payload:-}" ]]; then
    "$embed_payload"
fi
"${cc[@]}" "${link_flags[@]}" -Wl,--build-id=none \
    -T os/boot/linker.ld -o "build/$image_name.elf" "${objs[@]}" $RUNTIME_LIB
$OBJCOPY -O binary "build/$image_name.elf" "build/$image_name.bin"
echo "linked $(wc -c < "build/$image_name.bin") bytes"
$NM "build/$image_name.elf" | grep -E "lvgl_demo_run|__image_end" || true
