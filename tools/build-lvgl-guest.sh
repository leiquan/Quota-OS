#!/bin/bash
# LVGL guest. Upstream tree: third_party/lvgl (v9.3.0, see PIN.txt).
# Writes build/quotaos-lvgl.bin. After a QEMU check, that file is installed
# as prebuilt/quotaos.bin.
# Does not touch prebuilt/quotaos-serial.bin or history.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
source tools/guest-toolchain.sh
EXTRA_CFLAGS=${EXTRA_CFLAGS:-}
image_name=quotaos-lvgl
source_lists=(kernel/sources.list)
embed_payload=
source_lists+=(port/wap/sources.list)
if [[ -n "${QUOTA_NODE_WEB_OUT:-}${QUOTA_WAP_PAGE_ELF:-}${QUOTA_WAP_INTERPRETER:-}" ]]; then
    source tools/wap-runtime.sh
    configure_wap_runtime
    if [[ -n "${QUOTA_WAP_PAGE_ELF:-}" ]]; then embed_payload=embed_wap_runtime; fi
fi
source tools/build-desktop-image.sh
