#!/bin/bash
# LVGL guest. Upstream tree: os/third_party/lvgl (v9.3.0, see PIN.txt).
# Writes build/quotaos-lvgl.bin. After a QEMU check, that file is installed
# as prebuilt/quotaos.bin.
# Does not touch prebuilt/quotaos-serial.bin or history.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
source tools/guest-toolchain.sh
EXTRA_CFLAGS="-DQUOTA_SYSTEM_STATUS ${EXTRA_CFLAGS:-}"
image_name=quotaos-lvgl
source_lists=(os/kernel/sources.list)
embed_payload=
source_lists+=(os/port/wap/sources.list)
volume_default=1
if [[ -n "${QUOTA_NODE_WEB_OUT:-}${QUOTA_WAP_PAGE_ELF:-}${QUOTA_WAP_INTERPRETER:-}" ]]; then volume_default=0; fi
if [[ "${QUOTA_RUNTIME_VOLUME_BOOT:-$volume_default}" == 1 ]]; then
    image_name=quotaos-node-web-volume
    EXTRA_CFLAGS="$EXTRA_CFLAGS -DQUOTA_RUNTIME_VOLUME_BOOT"
    if [[ "${QUOTA_RUNTIME_VOLUME_LINUX_ABI:-1}" == 1 ]]; then
        # Compile process support without emitting embedded ELF or DSO blobs.
        EXTRA_CFLAGS="$EXTRA_CFLAGS -DQUOTA_NODE_WEB -DQUOTA_WAP_FULL_RUNTIME -DQUOTA_CALLER_ACCOUNTED_SVC"
    fi
elif [[ -n "${QUOTA_NODE_WEB_OUT:-}${QUOTA_WAP_PAGE_ELF:-}${QUOTA_WAP_INTERPRETER:-}" ]]; then
    source tools/wap-runtime.sh
    configure_wap_runtime
    if [[ -n "${QUOTA_WAP_PAGE_ELF:-}" ]]; then embed_payload=embed_wap_runtime; fi
fi
source tools/build-desktop-image.sh
