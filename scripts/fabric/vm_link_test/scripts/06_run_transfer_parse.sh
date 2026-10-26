#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

echo "Receiver data-link IP: $RECEIVER_DATA_IP"

RECEIVER_CMD="rm -f '$REMOTE_DATA_PATH' '$REMOTE_RUN_DIR/parse_report.txt' '$REMOTE_RUN_DIR/receiver.log'; mkdir -p '$REMOTE_RUN_DIR'; nohup sh -c 'nc -l -p $REMOTE_PORT > $REMOTE_DATA_PATH && cd $REMOTE_SRC_DIR && ./itchy $REMOTE_DATA_PATH --benchmark > $REMOTE_RUN_DIR/parse_report.txt 2>&1' > '$REMOTE_RUN_DIR/receiver.log' 2>&1 & echo \$! > '$REMOTE_RUN_DIR/receiver.pid'"
run loomai exec "$SLICE_NAME" "$RECEIVER_CMD" --nodes "$RECEIVER_NODE"

SENDER_CMD="test -f '$REMOTE_DATA_PATH' && timeout 3600 sh -c 'cat $REMOTE_DATA_PATH | nc -N $RECEIVER_DATA_IP $REMOTE_PORT'"
run loomai exec "$SLICE_NAME" "$SENDER_CMD" --nodes "$SENDER_NODE"

WAIT_CMD="sh -c 'for i in $(seq 1 120); do if test -s "$0/parse_report.txt"; then cat "$0/parse_report.txt"; exit 0; fi; sleep 5; done; echo timed-out-waiting-for-parse-report; cat "$0/receiver.log"; exit 1' '$REMOTE_RUN_DIR'"
run loomai exec "$SLICE_NAME" "$WAIT_CMD" --nodes "$RECEIVER_NODE"
