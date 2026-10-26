#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

require_cmd loomai
require_cmd python3
require_cmd tar
require_file "$LOCAL_DATA_FILE"
require_file "main.c"
require_file "parser.c"
require_file "Makefile"

python3 - <<'PYCODE'
import gzip
from pathlib import Path
path = Path('data/tvagg.gz')
with path.open('rb') as f:
    magic = f.read(3)
print(f'compressed_magic={magic.hex()}')
with gzip.open(path, 'rb') as f:
    first = f.read(12)
if len(first) < 12:
    raise SystemExit('sample data is too short')
msg_len = int.from_bytes(first[:2], 'big')
msg_type = chr(first[2])
print(f'first_record_length={msg_len}')
print(f'first_record_type={msg_type}')
PYCODE

echo "Local preflight OK. No FABRIC resources were changed."
