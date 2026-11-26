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
printf 'before-ext4-write' > "$work/root/scratch.txt"
export LC_ALL=C
mke2fs=${MKE2FS:-mke2fs}
dumpe2fs=${DUMPE2FS:-dumpe2fs}
"$mke2fs" -V
features=none,filetype,extent
blocks=2048
journal=()
if [[ ${CLI_ROOT_JOURNAL:-0} == 1 ]]; then
    features+=,has_journal
    blocks=4096
    journal=(-J size=4)
fi
"$mke2fs" -q -F -t ext4 -b 4096 -I 256 -O "$features" \
    -E lazy_itable_init=0,lazy_journal_init=0 ${journal[@]+"${journal[@]}"} \
    -d "$work/root" "$work/root.ext4" "$blocks"
"$dumpe2fs" -h "$work/root.ext4" | awk '/^Filesystem features|^Filesystem state|^Block size|^Inode size/'
mv "$work/root.ext4" "$output"
if command -v sha256sum >/dev/null; then
    sha256sum "$output"
else
    shasum -a 256 "$output"
fi
