#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

if [[ "${CONFIRM_DELETE:-}" != "yes" ]]; then
  echo "Refusing to delete slice $SLICE_NAME without CONFIRM_DELETE=yes" >&2
  echo "Run: CONFIRM_DELETE=yes bash scripts/fabric/vm_link_test/scripts/08_cleanup_slice.sh" >&2
  exit 1
fi

run loomai slices delete "$SLICE_NAME"
