#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

run loomai slices submit "$SLICE_NAME" --wait --timeout "$SUBMIT_TIMEOUT"
run loomai slices show "$SLICE_NAME"
