#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

run loomai slices show "$SLICE_NAME"
run loomai slices slivers "$SLICE_NAME"
