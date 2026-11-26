#!/bin/bash
set -euo pipefail

qemu=$(command -v qemu-system-aarch64) || {
    echo 'qemu-system-aarch64 is required.' >&2
    exit 127
}
if [[ $(uname -s) != Darwin ]]; then
    exec "$qemu" "$@"
fi

tools_dir=$(cd "$(dirname "$0")" && pwd)
bundle="$tools_dir/../build/Quota OS.app"
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
install_if_changed "$tools_dir/macos/QuotaOS.Info.plist" "$bundle/Contents/Info.plist"
exec "$bundle/Contents/MacOS/qemu-system-aarch64" "$@"
