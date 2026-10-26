#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

run loomai slices create "$SLICE_NAME"
run loomai nodes add "$SLICE_NAME" "$SENDER_NODE" --site "$SITE" --cores "$CORES" --ram "$RAM" --disk "$DISK" --image "$IMAGE"
run loomai nodes add "$SLICE_NAME" "$RECEIVER_NODE" --site "$SITE" --cores "$CORES" --ram "$RAM" --disk "$DISK" --image "$IMAGE"
run loomai components add "$SLICE_NAME" "$SENDER_NODE" nic1 --model NIC_Basic
run loomai components add "$SLICE_NAME" "$RECEIVER_NODE" nic1 --model NIC_Basic
run loomai networks add "$SLICE_NAME" data-link --type L2Bridge -i "$SENDER_NODE-nic1-p1,$RECEIVER_NODE-nic1-p1"
run loomai slices validate "$SLICE_NAME"

echo "Draft L2 test slice created and validated: $SLICE_NAME"
echo "No resources are provisioned until you run 02_submit_slice.sh."
