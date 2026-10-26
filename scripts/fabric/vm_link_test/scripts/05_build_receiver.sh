#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

BUILD_CMD="cd '$REMOTE_SRC_DIR' && make clean >/dev/null 2>&1 || true; cd '$REMOTE_SRC_DIR' && make"
run loomai exec "$SLICE_NAME" "$BUILD_CMD" --nodes "$RECEIVER_NODE"
run loomai exec "$SLICE_NAME" "'$REMOTE_SRC_DIR'/itchy '$REMOTE_DATA_PATH' --benchmark || true" --nodes "$RECEIVER_NODE"

echo "Build complete. The benchmark command above succeeds when the receiver already has $REMOTE_DATA_PATH; otherwise it reports io_error=2."
