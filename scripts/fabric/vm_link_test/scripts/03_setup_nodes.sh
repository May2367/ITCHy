#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

INSTALL_CMD='sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential zlib1g-dev uthash-dev netcat-openbsd gzip tar time'
run loomai exec "$SLICE_NAME" "$INSTALL_CMD" --nodes "$SENDER_NODE,$RECEIVER_NODE" --parallel
run loomai exec "$SLICE_NAME" "mkdir -p '$REMOTE_RUN_DIR'" --nodes "$SENDER_NODE,$RECEIVER_NODE" --parallel
run loomai exec "$SLICE_NAME" "sudo mkdir -p '$REMOTE_SRC_DIR' && sudo chown -R \$(id -un):\$(id -gn) '$REMOTE_SRC_DIR'" --nodes "$RECEIVER_NODE"

CONFIGURE_SENDER="test -d /sys/class/net/enp7s0; sudo ip link set enp7s0 up; sudo ip addr flush dev enp7s0; sudo ip addr add '$SENDER_DATA_IP/$DATA_PREFIX' dev enp7s0; ip -br addr show enp7s0"
CONFIGURE_RECEIVER="test -d /sys/class/net/enp7s0; sudo ip link set enp7s0 up; sudo ip addr flush dev enp7s0; sudo ip addr add '$RECEIVER_DATA_IP/$DATA_PREFIX' dev enp7s0; ip -br addr show enp7s0"
run loomai exec "$SLICE_NAME" "$CONFIGURE_SENDER" --nodes "$SENDER_NODE"
run loomai exec "$SLICE_NAME" "$CONFIGURE_RECEIVER" --nodes "$RECEIVER_NODE"
run loomai exec "$SLICE_NAME" "ping -c 3 '$RECEIVER_DATA_IP'" --nodes "$SENDER_NODE"
