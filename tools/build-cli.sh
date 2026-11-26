#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/guest-toolchain.sh
out=build/cli
mkdir -p "$out" build
printf '#define QUOTAOS_COMMIT "%s"\n' "$(git rev-parse --short HEAD)" > "$out/commit.h"
cli_flags=(-DQUOTA_CLI)
if [[ ${CLI_VERBOSE_BOOT:-0} == 1 ]]; then cli_flags+=(-DQUOTA_VERBOSE_BOOT); fi
objs=()
while IFS= read -r f; do
    case "$f" in kernel/demo.c|kernel/apps/*|user/*|kernel/drivers/fb.c|kernel/drivers/input.c|kernel/lvgl_boot.c|kernel/isolate.c|kernel/e4.c) continue;; esac
    obj="$out/${f%.*}.o"; mkdir -p "$(dirname "$obj")"
    "${cc[@]}" "${cli_flags[@]}" -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -mgeneral-regs-only -mstrict-align -ffunction-sections -fdata-sections -Iinclude -I"$out" -c "$f" -o "$obj"
    objs+=("$obj")
done < <(cat kernel/sources.list; echo cli/console.c)
sed '/__image_end =/i\
    .cli_module (NOLOAD) : ALIGN(4096) { __cli_module = .; . += 4096; __cli_module_size = .; . += 4; __cli_source = .; . += 4096; } :data
' boot/linker.ld > "$out/linker.ld"
"${cc[@]}" "${link_flags[@]}" -Wl,--gc-sections,--build-id=none -T "$out/linker.ld" -o "$out/kernel.elf" "${objs[@]}" $RUNTIME_LIB
"$OBJCOPY" -O binary "$out/kernel.elf" build/quotaos-cli.bin
cp cli/test.c "$out/test.c"
"${cc[@]}" "${link_flags[@]}" -Wl,--build-id=none -O2 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mgeneral-regs-only -T user/user.ld "$out/test.c" -o "$out/test.elf"
"$OBJCOPY" -O binary "$out/test.elf" "$out/test.bin"
"$NM" "$out/kernel.elf" | awk '$3=="__cli_module" {print "0x"$1}' > "$out/module.addr"
