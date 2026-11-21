#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
LLVM_PREFIX=${LLVM_PREFIX:-$(brew --prefix llvm)}
LLD_PREFIX=${LLD_PREFIX:-$(brew --prefix lld)}
export PATH="$LLD_PREFIX/bin:$PATH"
export CC="$LLVM_PREFIX/bin/clang --target=aarch64-none-elf -fuse-ld=lld"
export CXX="$LLVM_PREFIX/bin/clang++ --target=aarch64-none-elf -fuse-ld=lld -nostdinc++"
export OBJCOPY="$LLVM_PREFIX/bin/llvm-objcopy"
export NM="$LLVM_PREFIX/bin/llvm-nm"
export EXTRA_CFLAGS="-Wno-unknown-attributes -Wno-unused-command-line-argument -Itools/freestanding"
export RUNTIME_LIB=' '
exec bash tools/build-lvgl-guest.sh
