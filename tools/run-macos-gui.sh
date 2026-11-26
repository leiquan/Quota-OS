#!/bin/bash
# Apple Silicon Cocoa launcher. Set QUOTA_TOUCH_HVF=1 for the native-clock diagnostic path.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
    echo 'This launcher requires an Apple Silicon Mac.' >&2
    exit 1
fi
if [[ $# -gt 0 && $1 != -* ]]; then
    export QUOTA_KERNEL=$1
    shift
fi
exec /bin/bash tools/run-files.sh -display cocoa,full-screen=off,zoom-to-fit=off,show-cursor=off "$@"
