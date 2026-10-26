#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

require_file "$LOCAL_DATA_FILE"
mkdir -p "$TEST_DIR/.build"
ARCHIVE="$TEST_DIR/.build/itchy-src.tar.gz"

run tar -czf "$ARCHIVE" src include tests Makefile README.md docs .github data

run loomai scp "$SLICE_NAME" "$RECEIVER_NODE" "$ARCHIVE" /tmp/itchy-src.tar.gz
run loomai exec "$SLICE_NAME" "rm -rf '$REMOTE_SRC_DIR'/* && tar -xzf /tmp/itchy-src.tar.gz -C '$REMOTE_SRC_DIR'" --nodes "$RECEIVER_NODE"

run loomai exec "$SLICE_NAME" "mkdir -p '$REMOTE_RUN_DIR'" --nodes "$SENDER_NODE"

echo "Uploaded source to $RECEIVER_NODE:$REMOTE_SRC_DIR"
echo "Run 04b_upload_data_chunks.sh to upload the feed to $SENDER_NODE:$REMOTE_DATA_PATH"
