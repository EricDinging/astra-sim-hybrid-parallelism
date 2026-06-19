#!/bin/bash
# farm_cal_item.sh <host> <model> <shape> <size> — run one isolated
# calibration sim remotely and store the result as a local shard CSV.
set -euo pipefail
HOST="$1"; MODEL="$2"; SHAPE="$3"; SIZE="$4"
HERE=$(dirname "$(realpath "$0")"); SWEEP=$(realpath "$HERE/..")
REMOTE_DIR="${REMOTE_DIR:-/workspace/run/sweep16}"
SSHOPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=15"
mkdir -p "$SWEEP/calibration/shards"
line=$(ssh $SSHOPTS "$HOST" \
  "bash $REMOTE_DIR/scripts/cal_one.sh $MODEL $SHAPE $SIZE" | tail -1)
[[ "$line" == "$MODEL,$SHAPE,$SIZE,"* ]] || {
  echo "bad cal_one output for $MODEL/$SHAPE on $HOST: '$line'" >&2; exit 1; }
echo "$line" > "$SWEEP/calibration/shards/${MODEL}_${SHAPE}.csv"
echo "shard ${MODEL}_${SHAPE}: $line"
