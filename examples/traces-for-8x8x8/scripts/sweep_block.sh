#!/bin/bash
# sweep_block.sh — rfold block-size sweep over the 8x8x8 traces.
#
# Grid (override any via env):
#   CELLS       cells to sweep        (default: all 8, heavy-first)
#   BLOCKS      --block-size values   (default: 1x1x1 2x2x2)
#   ADMISSIONS  admission policies    (default: fifo easy)
#   POLICIES    placement policies    (default: rfold)
#
# Only rfold is swept: block geometry is a property of the OCS hardware, and
# rfold is the only policy that reconfigures. --block-size is rfold's primary
# knob; the old standalone "folding" policy is now rfold-full (block pinned to
# the whole torus), block-invariant by definition, with its canonical result in
# the baseline run. The remaining 5 placements never read --block-size at all,
# so the existing output/<admission>/<placement>/ baselines stand for every block.
#
# Reuses configs/, mixlib/, calibration/cells.csv and the BLOCK-aware
# scripts/run_policy.sh. Servers are prepared by copying the prebuilt binary +
# mixlib/configs/scripts (no remote build), per the project farm method.
#
# Outputs (merged locally, never wiped) — flat per-run folders:
#   results/<placement>-<admission>-<cell-dashed>-<B>-0/
#     {jobs.csv, summary.txt, node_jobs.csv, run.log}
# rfold at B=8x8x8 IS the folding arm, so that point lands under the
# canonical folding-...-8x8x8-0 name; rfold at the binary-default B=4x4x4
# is the baseline rfold-...-4x4x4-0 run. Both are skipped when present.
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
REMOTE_DIR="${REMOTE_DIR:-/workspace/run/blocksweep}"
SSHOPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=15"

read -r -a CELLS <<< "${CELLS:-bw_heavy_large bw_light_large bw_heavy_small bw_light_small lat_heavy_large lat_light_large lat_heavy_small lat_light_small}"
read -r -a BLOCKS     <<< "${BLOCKS:-1x1x1 2x2x2}"
read -r -a ADMISSIONS <<< "${ADMISSIONS:-fifo easy}"
read -r -a POLICIES   <<< "${POLICIES:-rfold}"
N_JOBS=400; SEED=1; BW_MAX=9999

# Per-cell trace mix — MUST match the calibration that produced cells.csv.
WEIGHTS_SMALL="4:0.25,8:0.125,16:0.0625"
WEIGHTS_LARGE="64:0.015625,128:0.0078125,256:0.00390625"
declare -A BF=( [lat_light_small]=0 [lat_light_large]=0 [lat_heavy_small]=0
                [lat_heavy_large]=0 [bw_light_small]=1 [bw_light_large]=1
                [bw_heavy_small]=1 [bw_heavy_large]=1 )
declare -A SW=( [lat_light_small]="$WEIGHTS_SMALL" [lat_heavy_small]="$WEIGHTS_SMALL"
                [bw_light_small]="$WEIGHTS_SMALL"  [bw_heavy_small]="$WEIGHTS_SMALL"
                [lat_light_large]="$WEIGHTS_LARGE" [lat_heavy_large]="$WEIGHTS_LARGE"
                [bw_light_large]="$WEIGHTS_LARGE"  [bw_heavy_large]="$WEIGHTS_LARGE" )

# mean inter-arrival (ns) for a cell; ignore the stray cells.csv row (empty $3).
cell_ia() { awk -F, -v c="$1" '$1==c && $3!=""{ia=$3} END{print ia}' "$CAL/cells.csv"; }

if [ -z "${REMOTE_HOSTS+x}" ]; then
  REMOTE_HOSTS=$(grep -v '^#' "$SWEEP/../../sshlist" 2>/dev/null | xargs || true)
fi
read -r -a HOSTS <<< "$REMOTE_HOSTS"

# Flat result-folder name: rfold pinned to the whole torus IS folding.
flat_name() {  # <cell> <admission> <placement> <block>
  local p="$3"
  [ "$p" = "rfold" ] && [ "$4" = "8x8x8" ] && p=folding
  echo "$p-$2-${1//_/-}-$4-0"
}

RSH() { local h="$1"; shift; ssh $SSHOPTS "$h" "$@"; }

setup_host() {
  local h="$1"
  RSH "$h" "sudo mkdir -p $REMOTE_DIR && sudo chown -R \$(id -u):\$(id -g) $REMOTE_DIR \
            && mkdir -p $REMOTE_DIR/bin $REMOTE_DIR/cells" || return 1
  RSH "$h" "sudo apt-get install -y -q libprotobuf23 libscotch-6.1 libhwloc15 pigz >/dev/null 2>&1 || true"
  # rm first: overwriting a possibly exec-mapped binary hits ETXTBSY;
  # unlinking is always allowed and scp then writes a fresh inode.
  RSH "$h" "rm -f $REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable" || return 1
  scp $SSHOPTS -q "$BIN" "$h:$REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable" || return 1
  # The exact local binary must land remotely (it carries the 1x1x1
  # directions_disjoint fix that is not in any committed build).
  local lmd5 rmd5
  lmd5=$(md5sum "$BIN" | cut -d' ' -f1)
  rmd5=$(RSH "$h" "md5sum $REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable" | cut -d' ' -f1)
  [ "$lmd5" = "$rmd5" ] || { echo "[setup] $h: binary md5 mismatch after scp" >&2; return 1; }
  if RSH "$h" "ldd $REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable 2>/dev/null | grep -q 'not found'"; then
    echo "[setup] $h: binary has unresolved libs:" >&2
    RSH "$h" "ldd $REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable | grep 'not found'" >&2
    return 1
  fi
  tar -C "$SWEEP" -chf - scripts configs | RSH "$h" "tar -C $REMOTE_DIR -xf -" || return 1
  # Copy mixlib only when missing: re-extracting over a live mixlib races any
  # concurrently running sim that reads these traces (jobs/ symlink into it).
  if ! RSH "$h" "test -f $REMOTE_DIR/mixlib/lat/1x1x4/chakra_trace.0.et"; then
    tar -C "$SWEEP" -cf - mixlib | pigz -1 | RSH "$h" "cd $REMOTE_DIR && pigz -d | tar -xf -" || return 1
  fi
  RSH "$h" "test -f $REMOTE_DIR/mixlib/lat/1x1x4/chakra_trace.0.et" \
    && echo "[setup] $h ready" || { echo "[setup] $h FAILED" >&2; return 1; }
}

ensure_cell() {  # <host> <cell>  — regenerate once per host per cell (cached)
  local h="$1" name="$2" ia bf sw
  ia=$(cell_ia "$name"); bf="${BF[$name]}"; sw="${SW[$name]}"
  [ -n "$ia" ] || { echo "ERROR: no ia for $name in cells.csv" >&2; return 1; }
  RSH "$h" "cd $REMOTE_DIR; test -s cells/$name/arrivals.csv || { ulimit -n 1048576;
    python3 scripts/gen_mixed.py --out cells/$name --lib mixlib --n $N_JOBS --seed $SEED \
      --bw-frac $bf --bw-max-size $BW_MAX --size-weights '$sw' --mean-ia-ns $ia >/dev/null; }" || return 1
  # arrival files must be byte-identical local vs remote (frozen plane).
  local lmd5 rmd5
  lmd5=$(md5sum "$TRACES/$name/arrivals.csv" | cut -d' ' -f1)
  rmd5=$(RSH "$h" "md5sum $REMOTE_DIR/cells/$name/arrivals.csv" | cut -d' ' -f1)
  [ "$lmd5" = "$rmd5" ] || { echo "ERROR: $name arrivals.csv local/remote mismatch on $h" >&2; return 1; }
}

run_cell() {  # <host> <cell>  — all missing block x admission x placement combos
  local h="$1" name="$2" B a p fail=0
  ensure_cell "$h" "$name" || { echo "ERROR: $name cell gen failed on $h" >&2; return 1; }
  # Flatten the cell's missing combos and run them all concurrently (<=8 sims,
  # each single-threaded; same envelope as the 7-policy sweep).
  local combos=()
  for B in "${BLOCKS[@]}"; do
    for a in "${ADMISSIONS[@]}"; do
      for p in "${POLICIES[@]}"; do
        [ -s "$RESULTS/$(flat_name "$name" "$a" "$p" "$B")/jobs.csv" ] || combos+=("$B/$a/$p")
      done
    done
  done
  [ ${#combos[@]} -eq 0 ] && { echo "[run] $name: nothing missing"; return 0; }
  echo "[run] $name on $h: ${combos[*]}"
  RSH "$h" "cd $REMOTE_DIR; ulimit -n 1048576
    pids=; bfail=0
    for c in ${combos[*]}; do
      B=\${c%%/*}; rest=\${c#*/}; a=\${rest%%/*}; p=\${rest#*/}
      BLOCK=\$B LABEL=block\$B/\$a/\$p \
        ASTRA_SIM=$REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable CFG=$REMOTE_DIR/configs \
        bash scripts/run_policy.sh $REMOTE_DIR/cells/$name \$a \$p & pids=\"\$pids \$!\"
    done
    for pid in \$pids; do wait \$pid || bfail=1; done
    exit \$bfail" || { echo "WARNING: $name: >=1 sim failed on $h (logs remote)" >&2; fail=1; }
  local c dst
  for c in "${combos[@]}"; do
    B=${c%%/*}; a=$(echo "$c" | cut -d/ -f2); p=${c##*/}
    dst="$RESULTS/$(flat_name "$name" "$a" "$p" "$B")"
    mkdir -p "$dst"
    rsync -az -e "ssh $SSHOPTS" "$h:$REMOTE_DIR/cells/$name/output/block$B/$a/$p/" \
      "$dst/" || fail=1
    if [ ! -s "$dst/jobs.csv" ] ||
       ! grep -q "^admission_policy:[[:space:]]*$a\$" "$dst/summary.txt" 2>/dev/null ||
       ! grep -q "^placement_policy:[[:space:]]*$p\$" "$dst/summary.txt" 2>/dev/null; then
      echo "ERROR: bad/missing results for $name/block$B/$a/$p" >&2; fail=1
    fi
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
  echo "[run] ${#CELLS[@]} cells over ${#HOSTS[@]} hosts; per cell:" \
       "${#BLOCKS[@]} blocks x ${#ADMISSIONS[@]} adm x ${#POLICIES[@]} plc"
  declare -A Q=()
  local i=0 name h
  for name in "${CELLS[@]}"; do
    h="${HOSTS[$((i % ${#HOSTS[@]}))]}"; Q[$h]="${Q[$h]:-} $name"; i=$((i+1))
  done
  local pids=() fail=0
  for h in "${HOSTS[@]}"; do
    [ -n "${Q[$h]:-}" ] || continue
    ( qf=0
      for name in ${Q[$h]}; do
        echo "[run] cell $name -> $h"
        run_cell "$h" "$name" || qf=1
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
