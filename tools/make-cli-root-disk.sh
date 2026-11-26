#!/bin/bash
set -euo pipefail
if [[ $# != 1 ]]; then
    echo "Usage: $0 OUTPUT.ext4" >&2
    exit 2
fi

output=$1
if [[ $output != /* ]]; then output=$PWD/$output; fi
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/root"
printf 'quota-ext4-readonly-fixture\n' > "$work/root/hello.txt"
cp build/cli/test.elf "$work/root/Test.elf"
export LC_ALL=C
mke2fs=${MKE2FS:-mke2fs}
dumpe2fs=${DUMPE2FS:-dumpe2fs}
"$mke2fs" -V
"$mke2fs" -q -F -t ext4 -b 4096 -I 256 -O none,filetype,extent \
    -E lazy_itable_init=0 -d "$work/root" "$work/root.ext4" 2048
"$dumpe2fs" -h "$work/root.ext4" | awk '/^Filesystem features|^Filesystem state|^Block size|^Inode size/'
mv "$work/root.ext4" "$output"
if command -v sha256sum >/dev/null; then
    sha256sum "$output"
else
    shasum -a 256 "$output"
fi
