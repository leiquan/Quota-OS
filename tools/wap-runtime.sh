# Sourced by the guest image builder when a WAP runtime is requested.
configure_wap_runtime() {
    if [ -n "${QUOTA_NODE_WEB_OUT:-}" ]; then
        : "${QUOTA_WAP_INTERPRETER:?Set the target ARM64 interpreter path}"
        : "${QUOTA_SYSTEM_FONT:?Set the target TrueType font path}"
        if [ -n "${QUOTA_ELECTRON_ASSETS:-}" ]; then
            echo "Choose one runtime for this image" >&2
            exit 2
        fi
        for artifact in node quota-blink libv8.so snapshot_blob.bin icudtl.dat; do
            if [ ! -s "$QUOTA_NODE_WEB_OUT/$artifact" ]; then
                echo "Missing joint build artifact: $QUOTA_NODE_WEB_OUT/$artifact" >&2
                exit 1
            fi
        done
        QUOTA_WAP_PAGE_ELF="$QUOTA_NODE_WEB_OUT/node"
        QUOTA_WAP_LIBRARY_PATH="$QUOTA_NODE_WEB_OUT${QUOTA_WAP_LIBRARY_PATH:+:$QUOTA_WAP_LIBRARY_PATH}"
        QUOTA_WAP_FULL_RUNTIME=1
        EXTRA_CFLAGS="$EXTRA_CFLAGS -DQUOTA_NODE_WEB"
        image_name=quotaos-node-web
    fi
    if [ -n "${QUOTA_WAP_INTERPRETER:-}" ]; then
        : "${QUOTA_WAP_PAGE_ELF:?A page ELF is required with its interpreter}"
        EXTRA_CFLAGS="$EXTRA_CFLAGS -DQUOTA_WAP_INTERPRETER"
        if [ -n "${QUOTA_ELECTRON_ASSETS:-}" ]; then
            EXTRA_CFLAGS="$EXTRA_CFLAGS -DQUOTA_ELECTRON_RUNTIME"
        fi
        if [ "${QUOTA_WAP_FULL_RUNTIME:-0}" = 1 ]; then
            EXTRA_CFLAGS="$EXTRA_CFLAGS -DQUOTA_WAP_FULL_RUNTIME"
        fi
    fi
}

embed_wap_runtime() {
    if [ -n "${QUOTA_NODE_WEB_OUT:-}" ]; then
        "$OBJCOPY" --strip-unneeded "$QUOTA_WAP_PAGE_ELF" build/wap-worker.elf
    else
        cp "$QUOTA_WAP_PAGE_ELF" build/wap-worker.elf
    fi
    cat >build/wap-worker.S <<'ASM'
.section .rodata.wap_worker,"a",%progbits
.balign 4096
.global wap_worker_start, wap_worker_end
wap_worker_start:
.incbin "build/wap-worker.elf"
wap_worker_end:
ASM
    if [ -n "${QUOTA_WAP_INTERPRETER:-}" ]; then
        cp "$QUOTA_WAP_INTERPRETER" build/wap-interp.elf
        cat >>build/wap-worker.S <<'ASM'
.balign 4096
.global wap_interp_start, wap_interp_end
wap_interp_start:
.incbin "build/wap-interp.elf"
wap_interp_end:
ASM
    fi
    if [ -n "${QUOTA_WAP_LIBRARY_PATH:-}" ]; then
        assets=()
        if [ -n "${QUOTA_ELECTRON_ASSETS:-}" ]; then
            assets=(--electron-assets "$QUOTA_ELECTRON_ASSETS"
                    --system-font "${QUOTA_SYSTEM_FONT:?Set QUOTA_SYSTEM_FONT to a readable TrueType font}")
        fi
        if [ -n "${QUOTA_NODE_WEB_OUT:-}" ]; then
            assets=(--strip-with-objcopy "$OBJCOPY" --blink-renderer "$QUOTA_NODE_WEB_OUT/quota-blink"
                    --v8-data "$QUOTA_NODE_WEB_OUT" --system-font "$QUOTA_SYSTEM_FONT")
        fi
        if [ -n "${QUOTA_WAP_INTERPRETER:-}" ]; then
            assets+=(--embedded-interpreter "$QUOTA_WAP_INTERPRETER")
        fi
        IFS=: read -r -a dirs <<< "$QUOTA_WAP_LIBRARY_PATH"
        python3 tools/stage-wap-libraries.py "$QUOTA_WAP_PAGE_ELF" build/wap-libraries.S \
            "${dirs[@]}" "${assets[@]}" --readelf "${READELF:-llvm-readelf}"
        "${cc[@]}" -c build/wap-libraries.S -o build/wap-libraries.o
        objs+=(build/wap-libraries.o)
    fi
    "${cc[@]}" -c build/wap-worker.S -o build/wap-worker.o
    objs+=(build/wap-worker.o)
}
