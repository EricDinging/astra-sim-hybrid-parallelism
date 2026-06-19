#!/bin/bash
# sweep_failure.sh — NPU-failure sweep over the 8x8x8 traces.
#
# Grid (override any via env):
#   CELLS       cells to sweep        (default: bw_heavy_large lat_light_small)
#   PROBS       --failure-prob values (default: 0.001 0.002 0.005 0.01)
#   ADMISSIONS  admission policies    (default: fifo easy)
#   POLICIES    placement policies    (default: all 7)
#
# Reuses configs/, mixlib/, calibration/cells.csv and the FAILURE_PROB-aware
# scripts/run_policy.sh. Servers are prepared by copying the prebuilt binary +
# mixlib/configs/scripts (no remote build), per the project farm method.
#
# Outputs (merged locally, never wiped) — flat per-run folders:
#   results/<placement>-<admission>-<cell-dashed>-<block>-<prob>/
#     {jobs.csv, summary.txt, node_jobs.csv, failed_nodes.log, run.log}
# Block is the binary default 4x4x4 (folding pins 8x8x8); this sweep does
# not vary it.
#
# Phases (positional, default all): prep | run | all
#   prep  set up every host (scp binary, install libs, copy mixlib/configs/tools)
#   run   regenerate cells remotely + run missing combos, rsync results back
# Idempotent: a combo with a local jobs.csv is skipped.
#
# Env: REMOTE_HOSTS (default: repo-root sshlist), REMOTE_DIR (default below).
set -uo pipefail
HERE=$(dirname "$(realpath "$0")")
SWEEP=$(realpath "$HERE/..")
cd "$SWEEP"

BIN="$SWEEP/../../build/astra_analytical/build/bin/AstraSim_Analytical_Reconfigurable"
TRACES="$SWEEP/traces"
RESULTS="$SWEEP/results"
CAL="$SWEEP/calibration"
REMOTE_DIR="${REMOTE_DIR:-/workspace/run/failsweep}"
SSHOPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=15"

read -r -a CELLS      <<< "${CELLS:-bw_heavy_large lat_light_small}"
read -r -a PROBS      <<< "${PROBS:-0.001 0.005 0.01}"
read -r -a ADMISSIONS <<< "${ADMISSIONS:-fifo easy}"
read -r -a POLICIES   <<< "${POLICIES:-firstfit random sfc l1clustering topomatch folding rfold}"
N_JOBS=400; SEED=1; BW_MAX=9999

# Per-cell trace mix — MUST match the calibration that produced cells.csv.
WEIGHTS_SMALL="4:0.25,8:0.125,16:0.0625"
WEIGHTS_LARGE="64:0.015625,128:0.0078125,256:0.00390625"
declare -A BF=( [lat_light_small]=0 [bw_heavy_large]=1 )
declare -A SW=( [lat_light_small]="$WEIGHTS_SMALL" [bw_heavy_large]="$WEIGHTS_LARGE" )

# mean inter-arrival (ns) for a cell; ignore the stray cells.csv row (empty $3).
cell_ia() { awk -F, -v c="$1" '$1==c && $3!=""{ia=$3} END{print ia}' "$CAL/cells.csv"; }

if [ -z "${REMOTE_HOSTS+x}" ]; then
  REMOTE_HOSTS=$(grep -v '^#' "$SWEEP/../../sshlist" 2>/dev/null | xargs || true)
fi
read -r -a HOSTS <<< "$REMOTE_HOSTS"

# Flat result-folder name; block is the in-effect default (folding = 8x8x8).
flat_name() {  # <cell> <admission> <placement> <prob>
  local B=4x4x4; [ "$3" = "folding" ] && B=8x8x8
  echo "$3-$2-${1//_/-}-$B-$4"
}

RSH() { local h="$1"; shift; ssh $SSHOPTS "$h" "$@"; }

setup_host() {
  local h="$1"
  RSH "$h" "sudo mkdir -p $REMOTE_DIR && sudo chown -R \$(id -u):\$(id -g) $REMOTE_DIR \
            && mkdir -p $REMOTE_DIR/bin $REMOTE_DIR/cells" || return 1
  RSH "$h" "sudo apt-get update -q >/dev/null 2>&1; \
            sudo apt-get install -y -q libprotobuf23 libscotch-6.1 libhwloc15 pigz >/dev/null 2>&1 || true"
  scp $SSHOPTS -q "$BIN" "$h:$REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable" || return 1
  if RSH "$h" "ldd $REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable 2>/dev/null | grep -q 'not found'"; then
    echo "[setup] $h: binary has unresolved libs:" >&2
    RSH "$h" "ldd $REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable | grep 'not found'" >&2
    return 1
  fi
  RSH "$h" "strings $REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable 2>/dev/null | grep -q failure-prob" \
    || { echo "[setup] $h: binary lacks --failure-prob (stale build)" >&2; return 1; }
  tar -C "$SWEEP" -chf - scripts configs | RSH "$h" "tar -C $REMOTE_DIR -xf -" || return 1
  tar -C "$SWEEP" -cf - mixlib | pigz -1 | RSH "$h" "cd $REMOTE_DIR && pigz -d | tar -xf -" || return 1
  RSH "$h" "test -f $REMOTE_DIR/mixlib/lat/1x1x4/chakra_trace.0.et" \
    && echo "[setup] $h ready" || { echo "[setup] $h FAILED" >&2; return 1; }
}

ensure_cell() {  # <host> <cell>  — regenerate once per host per cell (cached)
  local h="$1" name="$2" ia bf sw
  ia=$(cell_ia "$name"); bf="${BF[$name]}"; sw="${SW[$name]}"
  [ -n "$ia" ] || { echo "ERROR: no ia for $name in cells.csv" >&2; return 1; }
  RSH "$h" "cd $REMOTE_DIR; test -s cells/$name/arrivals.csv || { ulimit -n 1048576;
    python3 scripts/gen_mixed.py --out cells/$name --lib mixlib --n $N_JOBS --seed $SEED \
      --bw-frac $bf --bw-max-size $BW_MAX --size-weights '$sw' --mean-ia-ns $ia >/dev/null; }"
}

run_unit() {  # <host> <cell> <prob>  — 2 admissions x 7 placements, missing-only
  local h="$1" name="$2" prob="$3" a p plcs fail=0 dst
  ensure_cell "$h" "$name" || { echo "ERROR: $name cell gen failed on $h" >&2; return 1; }
  for a in "${ADMISSIONS[@]}"; do
    plcs=()
    for p in "${POLICIES[@]}"; do
      [ -s "$RESULTS/$(flat_name "$name" "$a" "$p" "$prob")/jobs.csv" ] || plcs+=("$p")
    done
    [ ${#plcs[@]} -eq 0 ] && continue
    echo "[run] $name fail=$prob $a x (${plcs[*]}) on $h"
    RSH "$h" "cd $REMOTE_DIR; ulimit -n 1048576
      pids=; bfail=0
      for p in ${plcs[*]}; do FAILURE_PROB=$prob LABEL=fail$prob/$a/\$p \
        ASTRA_SIM=$REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable CFG=$REMOTE_DIR/configs \
        bash scripts/run_policy.sh $REMOTE_DIR/cells/$name $a \$p & pids=\"\$pids \$!\"; done
      for pid in \$pids; do wait \$pid || bfail=1; done
      exit \$bfail" || { echo "WARNING: $name fail=$prob $a: >=1 sim failed on $h (logs remote)" >&2; fail=1; }
    for p in "${plcs[@]}"; do
      dst="$RESULTS/$(flat_name "$name" "$a" "$p" "$prob")"
      mkdir -p "$dst"
      rsync -az -e "ssh $SSHOPTS" "$h:$REMOTE_DIR/cells/$name/output/fail$prob/$a/$p/" \
        "$dst/" || fail=1
      # folding arm runs the binary's rfold policy at whole-torus block.
      bp="$p"; [ "$p" = "folding" ] && bp="rfold"
      if [ ! -s "$dst/jobs.csv" ] ||
         ! grep -q "^admission_policy:[[:space:]]*$a\$" "$dst/summary.txt" 2>/dev/null ||
         ! grep -q "^placement_policy:[[:space:]]*$bp\$" "$dst/summary.txt" 2>/dev/null; then
        echo "ERROR: bad/missing results for $name/fail$prob/$a/$p" >&2; fail=1
      fi
    done
  done
  return "$fail"
}

do_prep() {
  [ ${#HOSTS[@]} -gt 0 ] || { echo "ERROR: no hosts (sshlist empty)" >&2; exit 1; }
  echo "[prep] setting up ${#HOSTS[@]} hosts in parallel"
  local pids=() h fail=0
  for h in "${HOSTS[@]}"; do setup_host "$h" & pids+=($!); done
  for p in "${pids[@]}"; do wait "$p" || fail=1; done
  [ "$fail" = 0 ] && echo "[prep] all hosts ready" || { echo "[prep] host setup FAILED" >&2; exit 1; }
}

do_run() {
  [ ${#HOSTS[@]} -gt 0 ] || { echo "ERROR: no hosts" >&2; exit 1; }
  # Work units = (cell, prob). CELLS order keeps the heavier cell first so it
  # spreads across hosts before the light cell piles on.
  local units=() name prob
  for name in "${CELLS[@]}"; do for prob in "${PROBS[@]}"; do units+=("$name|$prob"); done; done
  echo "[run] ${#units[@]} (cell,prob) units over ${#HOSTS[@]} hosts; "\
"${#ADMISSIONS[@]} adm x ${#POLICIES[@]} plc each"
  declare -A Q=()
  local i=0 u h
  for u in "${units[@]}"; do h="${HOSTS[$((i % ${#HOSTS[@]}))]}"; Q[$h]="${Q[$h]:-} $u"; i=$((i+1)); done
  local pids=() fail=0
  for h in "${HOSTS[@]}"; do
    [ -n "${Q[$h]:-}" ] || continue
    ( qf=0
      for u in ${Q[$h]}; do
        name="${u%%|*}"; prob="${u##*|}"
        echo "[run] unit $name fail=$prob -> $h"
        run_unit "$h" "$name" "$prob" || qf=1
      done
      exit "$qf" ) & pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p" || fail=1; done
  [ "$fail" = 0 ] && echo "[run] ALL UNITS OK" || echo "[run] some units reported failures (see warnings)" >&2
}

case "${1:-all}" in
  prep) do_prep ;;
  run)  do_run ;;
  all)  do_prep; do_run ;;
  *) echo "usage: $0 {prep|run|all}" >&2; exit 1 ;;
esac
