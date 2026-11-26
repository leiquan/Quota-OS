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
source_lists=(kernel/sources.list)
embed_payload=
source tools/build-desktop-image.sh
