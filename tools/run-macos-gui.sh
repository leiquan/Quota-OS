#!/bin/bash
# Apple Silicon: real host time with native CPU execution, not icount.
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
export QUOTA_TOUCH_HVF=1
exec /bin/bash tools/run-files.sh -display cocoa,full-screen=off,zoom-to-fit=off,show-cursor=off "$@"
