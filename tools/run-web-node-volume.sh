#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
export QUOTA_RUNTIME_VOLUME=${QUOTA_RUNTIME_VOLUME:-build/runtime.raw}
exec /bin/bash tools/run-files.sh "$@"
