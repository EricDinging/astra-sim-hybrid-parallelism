#!/bin/bash
# farm_run_item.sh <host> <cell> <admission> <policy> <block> <failprob>
#                  <bw_frac> <sizes> <one_node_frac> <mean_ia_ns> <n_jobs>
# One combo sim on HOST via scripts/run_one.sh (md5-aware on-host trace gen —
# self-healing when the probe loop rescales ia for the same cell name), then
# rsync the combo back. LOCAL_BASE (default traces) selects where the local
# reference cell lives (probe phase: probe-traces). With RESULTS_BASE set
# (the run phase), the combo lands in the flat
#   $RESULTS_BASE/<policy>-<admission>-<cell-dashes>-<block>-<failprob>/
# folder (8x8x8 convention); otherwise it returns to the nested
# <cell>/output/<policy>/ scratch layout (probe phase).
set -euo pipefail
HOST="$1"; CELL="$2"; ADMISSION="$3"; POLICY="$4"; BLOCKF="$5"; FAILP="$6"
BF="$7"; SIZES="$8"; ONF="$9"; IA="${10}"; N="${11}"
# Optional 12th field: gen_mixed --pmf-exp (truncated-Pareto exponent). Default
# 1.5 keeps every existing cell byte-identical; the reconfigurability sweep
# passes 0.0/0.5/1.5 to broaden the job-size mix.
PMF="${12:-1.5}"
HERE=$(dirname "$(realpath "$0")"); SWEEP=$(realpath "$HERE/..")
REMOTE_DIR="${REMOTE_DIR:-/workspace/run/sweep16}"
SSHOPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=15"
LOCAL_CELL="$SWEEP/${LOCAL_BASE:-traces}/$CELL"
COMBO="$POLICY-$ADMISSION-$(echo "$CELL" | tr _ -)-$BLOCKF-$FAILP"
if [ -n "${RESULTS_BASE:-}" ]; then
  DEST="$RESULTS_BASE/$COMBO"
else
  DEST="$LOCAL_CELL/output/$POLICY"
fi

# Handler-level idempotency: with results already on disk this item is a
# no-op. This is what lets auxiliary pools drain the queue tail while the
# main pool is running — when the main pool later reaches an item another
# pool completed, the dispatch returns instantly instead of re-simulating.
if [ -n "${RESULTS_BASE:-}" ] && [ -s "$DEST/jobs.csv" ]; then
  echo "skip $COMBO: results already present"
  exit 0
fi

LMD5=$(md5sum "$LOCAL_CELL/arrivals.csv" | cut -d' ' -f1)
ssh $SSHOPTS "$HOST" "PMF_EXP='$PMF' bash $REMOTE_DIR/scripts/run_one.sh '$CELL' \
  '$ADMISSION' '$POLICY' '$BLOCKF' '$FAILP' '$BF' '$SIZES' '$ONF' '$IA' '$N' \
  '$LMD5' '${KEEPLOGS:-0}'"

# arrivals byte-identity is enforced remotely against LMD5; re-verify anyway
rmd5=$(ssh $SSHOPTS "$HOST" \
  "md5sum $REMOTE_DIR/cells/$CELL/arrivals.csv" | cut -d' ' -f1)
[ "$LMD5" = "$rmd5" ] || {
  echo "arrivals.csv mismatch for $CELL on $HOST" >&2; exit 1; }

mkdir -p "$DEST"
rsync -az -e "ssh $SSHOPTS" \
  "$HOST:$REMOTE_DIR/cells/$CELL/output/$COMBO/" \
  "$DEST/"
test -s "$DEST/jobs.csv" || {
  echo "no jobs.csv came back for $COMBO" >&2; exit 1; }
echo "ok $COMBO from $HOST -> $DEST"
