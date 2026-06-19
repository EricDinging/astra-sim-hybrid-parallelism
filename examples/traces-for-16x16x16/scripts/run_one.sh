#!/bin/bash
# run_one.sh <cell> <admission> <policy> <block> <failprob> <bw_frac> <sizes>
#            <one_node_frac> <mean_ia_ns> <n_jobs> <want_md5> <keeplogs>
# Runs ON the farm host from the shipped REMOTE_DIR tree. Ensures the cell
# trace exists AND its arrivals.csv md5 matches want_md5 — regenerating if
# stale (e.g. the probe loop rescaled ia for the same cell name) — then runs
# one combo sim into the flat scratch folder
#   cells/<cell>/output/<policy>-<admission>-<cell-dashes>-<block>-<failprob>
# and prunes logs. The trace check/gen happens under a per-cell flock so
# concurrent pool slots running combos of the same cell don't race; the lock
# is released before the (long) sim.
set -euo pipefail
CELL="$1"; ADMISSION="$2"; POLICY="$3"; BLOCKF="$4"; FAILP="$5"
BF="$6"; SIZES="$7"; ONF="$8"; IA="$9"; N="${10}"
WANT_MD5="${11}"; KEEPLOGS="${12:-0}"
RD=$(realpath "$(dirname "$(realpath "$0")")/..")
cd "$RD"
ulimit -n 1048576
mkdir -p cells locks

exec 9>"locks/$CELL.lock"
flock 9
cur=""
if [ -f "cells/$CELL/arrivals.csv" ]; then
  cur=$(md5sum "cells/$CELL/arrivals.csv" | cut -d' ' -f1)
fi
if [ "$cur" != "$WANT_MD5" ]; then
  rm -rf "cells/$CELL"
  # --seed 1 must match reproduce.sh's SEED, and mixlib is where setup_host
  # ships the library — both are implicit contracts; the post-gen md5 assert
  # below catches any divergence.
  # PMF_EXP (env, default 1.5) lets the reconfigurability sweep regenerate the
  # broadened-size cells byte-identically; unset => default keeps every other
  # cell identical to before.
  python3 scripts/gen_mixed.py --out "cells/$CELL" --lib mixlib --n "$N" \
    --seed 1 --bw-frac "$BF" --sizes "$SIZES" --one-node-frac "$ONF" \
    --pmf-exp "${PMF_EXP:-1.5}" --mean-ia-ns "$IA" >/dev/null
  cur=$(md5sum "cells/$CELL/arrivals.csv" | cut -d' ' -f1)
  if [ "$cur" != "$WANT_MD5" ]; then
    echo "ERROR: regenerated cells/$CELL md5 $cur != expected $WANT_MD5" >&2
    exit 1
  fi
fi
flock -u 9

COMBO="$POLICY-$ADMISSION-$(echo "$CELL" | tr _ -)-$BLOCKF-$FAILP"
# BLOCK only matters to rfold (folding pins the whole torus itself); pass it
# only when off-default so default-flag invocations stay byte-identical.
EXTRA_ENV=()
if [ "$POLICY" = rfold ] && [ "$BLOCKF" != "4x4x4" ]; then
  EXTRA_ENV+=("BLOCK=$BLOCKF")
fi
if [ "$FAILP" != "0" ]; then
  EXTRA_ENV+=("FAILURE_PROB=$FAILP")
fi
env "${EXTRA_ENV[@]}" \
  OUT="$RD/cells/$CELL/output/$COMBO" \
  ASTRA_SIM="$RD/bin/AstraSim_Analytical_Reconfigurable" CFG="$RD/configs" \
  bash scripts/run_policy.sh "cells/$CELL" "$ADMISSION" "$POLICY"
if [ "$KEEPLOGS" = "0" ]; then
  find "cells/$CELL/output/$COMBO" -name '*.log' ! -name run.log \
    ! -name failed_nodes.log -delete 2>/dev/null || true
fi
