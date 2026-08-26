#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 root@BOARD_IP [REMOTE_DIR]" >&2
    exit 2
fi

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
REMOTE=$1
REMOTE_DIR=${2:-/root}
MODE=${MODE:-release}
BINARY="$PROJECT_ROOT/build/cross/riscv64/$MODE/licheerv-nano-rtsp"

if [[ ! -f "$BINARY" ]]; then
    echo "Missing $BINARY; run scripts/build.sh first." >&2
    exit 1
fi

scp "$BINARY" "$REMOTE:$REMOTE_DIR/licheerv-nano-rtsp"
ssh "$REMOTE" "chmod +x '$REMOTE_DIR/licheerv-nano-rtsp'"

if [[ -n ${SENSOR_CONFIG:-} ]]; then
    scp "$SENSOR_CONFIG" "$REMOTE:$REMOTE_DIR/sensor_cfg.ini"
fi

echo "Deployed to $REMOTE:$REMOTE_DIR/licheerv-nano-rtsp"

