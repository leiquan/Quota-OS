#!/bin/bash
set -euo pipefail

qemu=$(command -v qemu-system-aarch64) || {
    echo 'qemu-system-aarch64 is required.' >&2
    exit 127
}
# Preserve an explicit QEMU name supplied by a test or custom launch.
vm_name_set=0
for arg in "$@"; do
    case "$arg" in
        -name|--name|-name=*|--name=*) vm_name_set=1; break ;;
    esac
done
if [[ $vm_name_set == 0 ]]; then
    set -- -name 'Quota OS Touch UI' "$@"
fi

if [[ $(uname -s) != Darwin ]]; then
    exec "$qemu" "$@"
fi

tools_dir=$(cd "$(dirname "$0")" && pwd)
metadata="$tools_dir/macos/QuotaOS.Info.plist"
app_name=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleName' "$metadata")
bundle="$tools_dir/../build/$app_name.app"
mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources"

install_if_changed() {
    local source=$1 target=$2 staging
    if cmp -s "$source" "$target"; then
        return
    fi
    staging=$(mktemp "$target.XXXXXX")
    cp -p "$source" "$staging" && mv -f "$staging" "$target" || {
        rm -f "$staging"
        return 1
    }
}

# Run the installed QEMU with the project's macOS app identity and official mark.
install_if_changed "$qemu" "$bundle/Contents/MacOS/qemu-system-aarch64"
install_if_changed "$tools_dir/macos/QuotaOS.icns" "$bundle/Contents/Resources/QuotaOS.icns"
install_if_changed "$metadata" "$bundle/Contents/Info.plist"
exec "$bundle/Contents/MacOS/qemu-system-aarch64" "$@"
