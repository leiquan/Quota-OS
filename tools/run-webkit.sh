#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
exec qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 128M \
    -nic none -device ramfb -icount shift=4,sleep=off \
    -kernel prebuilt/quotaos-jsc.bin -append webkit -serial stdio -monitor none "$@"
