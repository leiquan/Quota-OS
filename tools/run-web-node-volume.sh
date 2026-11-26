#!/bin/bash
# Boot the external-runtime development image with a separate Notes disk.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "Usage: $0 READ_ONLY_RUNTIME_VOLUME NOTES_DISK" >&2
    exit 2
fi
export QUOTA_RUNTIME_VOLUME=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
export QUOTA_NOTES_DISK=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
exec /bin/bash "$(dirname "$0")/run-files.sh" -display "${QUOTA_DISPLAY:-default}"
