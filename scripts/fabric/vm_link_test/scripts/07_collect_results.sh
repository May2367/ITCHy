#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

OUT_DIR="$TEST_DIR/$RESULTS_DIR/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT_DIR"

loomai --format json slices show "$SLICE_NAME" > "$OUT_DIR/slice_show.json"
run loomai scp "$SLICE_NAME" "$RECEIVER_NODE" --download "$REMOTE_RUN_DIR/parse_report.txt" "$OUT_DIR/parse_report.txt"
run loomai scp "$SLICE_NAME" "$RECEIVER_NODE" --download "$REMOTE_RUN_DIR/receiver.log" "$OUT_DIR/receiver.log"
run loomai exec "$SLICE_NAME" "ls -lh '$REMOTE_RUN_DIR' && sha256sum '$REMOTE_DATA_PATH' || true" --nodes "$RECEIVER_NODE" > "$OUT_DIR/receiver_files.txt"
run loomai exec "$SLICE_NAME" "ls -lh '$REMOTE_RUN_DIR' && sha256sum '$REMOTE_DATA_PATH' || true" --nodes "$SENDER_NODE" > "$OUT_DIR/sender_files.txt"

echo "Results collected in $OUT_DIR"
