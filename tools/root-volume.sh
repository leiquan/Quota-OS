# Source after resolving the Notes disk and any read-only runtime volume.
root_args=()
if [[ ${QUOTA_ROOT_DISK-} != none ]]; then
    root_disk=${QUOTA_ROOT_DISK:-"$(dirname "$disk")/root.ext4"}
    if [[ ! -e "$root_disk" && -z ${QUOTA_ROOT_DISK:-} ]]; then
        mkfs=${MKE2FS:-mke2fs}
        if ! command -v "$mkfs" >/dev/null && [[ -x /opt/homebrew/opt/e2fsprogs/sbin/mke2fs ]]; then
            mkfs=/opt/homebrew/opt/e2fsprogs/sbin/mke2fs
        fi
        tmp_root=$(mktemp "${root_disk}.XXXXXX")
        trap 'rm -f "$tmp_root"' EXIT
        "$mkfs" -q -F -t ext4 -b 4096 -I 256 -O none,filetype,extent,has_journal -J size=4 -E lazy_itable_init=0,lazy_journal_init=0 "$tmp_root" 4096
        ln "$tmp_root" "$root_disk" || [[ -f "$root_disk" ]]
        rm -f "$tmp_root"
        trap - EXIT
    fi
    [[ -f "$root_disk" && ! "$root_disk" -ef "$disk" && ! "$root_disk" -ef "${runtime:-}" ]] || { echo "Select a separate ext4 root disk." >&2; exit 1; }
    root_args=(-drive "if=none,id=root,format=raw,file=${root_disk//,/,,}" -device virtio-blk-device,bus=virtio-mmio-bus.30,drive=root,serial=QUOTA-ROOT)
fi
