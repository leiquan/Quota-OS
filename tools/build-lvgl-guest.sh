#!/bin/bash
# LVGL guest. Upstream tree: os/third_party/lvgl (v9.3.0, see PIN.txt).
# Writes build/quotaos-lvgl.bin. After a QEMU check, that file is installed
# as prebuilt/quotaos.bin.
# Does not touch prebuilt/quotaos-serial.bin or history.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
source tools/guest-toolchain.sh
EXTRA_CFLAGS="${EXTRA_CFLAGS:-} -DQUOTA_SYSTEM_STATUS"
if [[ ${QUOTA_GUI_EXT4:-0} == 1 ]]; then
    EXTRA_CFLAGS="$EXTRA_CFLAGS -DQUOTA_GUI_EXT4"
fi
image_name=quotaos-lvgl
source_lists=(os/kernel/sources.list)
embed_payload=
source_lists+=(os/port/wap/sources.list)
if [[ ${QUOTA_RUNTIME_VOLUME_BOOT:-0} == 1 ]]; then
    image_name=quotaos-node-web-volume
    # Compile the Web UI Linux/WAP ABI without embedding the runtime libraries.
    EXTRA_CFLAGS="$EXTRA_CFLAGS -DQUOTA_RUNTIME_VOLUME_BOOT -DQUOTA_NODE_WEB -DQUOTA_WAP_FULL_RUNTIME -DQUOTA_CALLER_ACCOUNTED_SVC"
elif [[ -n "${QUOTA_NODE_WEB_OUT:-}${QUOTA_WAP_PAGE_ELF:-}${QUOTA_WAP_INTERPRETER:-}" ]]; then
    source tools/wap-runtime.sh
    configure_wap_runtime
    if [[ -n "${QUOTA_WAP_PAGE_ELF:-}" ]]; then embed_payload=embed_wap_runtime; fi
fi
source tools/build-desktop-image.sh
