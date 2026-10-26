#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

require_file "$LOCAL_DATA_FILE"
CHUNK_DIR="$TEST_DIR/.build/tvagg_chunks"
LOCAL_SHA_FILE="$TEST_DIR/.build/tvagg.sha256"
CHUNK_SIZE=$((32 * 1024 * 1024))
mkdir -p "$CHUNK_DIR"
rm -f "$CHUNK_DIR"/chunk.* "$LOCAL_SHA_FILE"

python3 - "$LOCAL_DATA_FILE" "$CHUNK_DIR" "$LOCAL_SHA_FILE" "$CHUNK_SIZE" <<'PYCODE'
import hashlib
import shutil
import sys
from pathlib import Path
src = Path(sys.argv[1])
out = Path(sys.argv[2])
sha_file = Path(sys.argv[3])
chunk_size = int(sys.argv[4])
h = hashlib.sha256()
idx = 0
with src.open('rb') as f:
    while True:
        data = f.read(chunk_size)
        if not data:
            break
        h.update(data)
        (out / f'chunk.{idx:04d}').write_bytes(data)
        idx += 1
sha_file.write_text(h.hexdigest() + '  ' + src.name + '\n')
print(f'chunks={idx}')
print(f'sha256={h.hexdigest()}')
PYCODE

run loomai exec "$SLICE_NAME" "rm -rf '$REMOTE_RUN_DIR/chunks' && mkdir -p '$REMOTE_RUN_DIR/chunks'" --nodes "$SENDER_NODE"
for chunk in "$CHUNK_DIR"/chunk.*; do
  run loomai scp "$SLICE_NAME" "$SENDER_NODE" "$chunk" "$REMOTE_RUN_DIR/chunks/$(basename "$chunk")"
done

REBUILD_CMD="cat '$REMOTE_RUN_DIR'/chunks/chunk.* > '$REMOTE_DATA_PATH' && sha256sum '$REMOTE_DATA_PATH'"
run loomai exec "$SLICE_NAME" "$REBUILD_CMD" --nodes "$SENDER_NODE"

echo "Local expected checksum:"
cat "$LOCAL_SHA_FILE"
