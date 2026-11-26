# ARM64 compiler, linker and binary utilities used by the guest image builder.
if [ -n "${QUOTA_LLVM_BIN:-}" ]; then
    cc=("$QUOTA_LLVM_BIN/clang" --target=aarch64-none-elf)
    link_flags=(-nostdlib -static "-fuse-ld=${QUOTA_LLD:-ld.lld}")
    OBJCOPY=${OBJCOPY:-"$QUOTA_LLVM_BIN/llvm-objcopy"}
    NM=${NM:-"$QUOTA_LLVM_BIN/llvm-nm"}
    READELF=${READELF:-"$QUOTA_LLVM_BIN/llvm-readelf"}
    RUNTIME_LIB=${RUNTIME_LIB:-}
    lv_headers=(-Itools/freestanding)
else
    cc=("${CC:-aarch64-linux-gnu-gcc}")
    link_flags=(-nostdlib -nostartfiles -static -no-pie)
    OBJCOPY=${OBJCOPY:-aarch64-linux-gnu-objcopy}
    NM=${NM:-aarch64-linux-gnu-nm}
    RUNTIME_LIB=${RUNTIME_LIB:--lgcc}
    lv_headers=()
fi
