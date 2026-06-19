#!/bin/bash
# reproduce.sh — Pareto-sized, load-calibrated 8x8x8 placement sweep.
#
# Spec: docs/superpowers/specs/2026-06-05-pareto-load-calibrated-sweep-design.md
# Phases (positional arg, default "all"):
#   lib        build the STG trace library (mixlib/, 2x47 entries) via Docker
#   calibrate  isolated service time per entry -> calibration/service_times.csv
#   gen        exact per-cell mean-ia from rho targets; generate 8 cells; gates
#   plan       print the missing-combo backfill plan and exit
#   run        farm-run MISSING admission x placement combos (sshlist hosts);
#              skip-existing makes this idempotent; existing results are never
#              touched and the verify step never rescales (fifo plane frozen)
#
# Results land in flat per-run folders (see results/README.md):
#   results/<placement>-<admission>-<bw|lat>-<heavy|light>-<small|large>-<block>-<failprob>/
# This script runs every combo at the binary's default block (4x4x4; folding
# pins 8x8x8) with no failures; block/failure sweeps live in scripts/.
#
# Env:
#   REMOTE_HOSTS  space-separated user@host list (default: repo-root sshlist).
#                 Set to "" to run cells locally (slow; smoke tests only).
#   STG_IMAGE     Docker image bundling STG   (default: astra:latest)
#   DOCKER        docker command              (default: "sudo docker")
#   REMOTE_DIR    remote working dir          (default: /workspace/run/sweep2)
#   MAXPAR_CAL    parallel calibration sims   (default: nproc-2)
#   KEEPLOGS      1 = keep per-policy run.log files on the farm copies
set -uo pipefail
HERE=$(dirname "$(realpath "$0")")
cd "$HERE"

LIB="$HERE/mixlib"
TRACES="$HERE/traces"
RESULTS="$HERE/results"
CAL="$HERE/calibration"
BIN="$HERE/../../build/astra_analytical/build/bin/AstraSim_Analytical_Reconfigurable"
CFG_DIR="$HERE/configs"
POLICIES=(firstfit random sfc l1clustering topomatch folding rfold)
ADMISSIONS=(fifo sjdf sjsf swf easy ljsf ljsfpack lwf)
N_JOBS=400; SEED=1; BW_MAX=9999
SIZES_SMALL="4,8,16";    WEIGHTS_SMALL="4:0.25,8:0.125,16:0.0625"
SIZES_LARGE="64,128,256"; WEIGHTS_LARGE="64:0.015625,128:0.0078125,256:0.00390625"
RHO_LIGHT=0.15; RHO_HEAVY=0.85
STG_IMAGE="${STG_IMAGE:-astra:latest}"
DOCKER="${DOCKER:-sudo docker}"
REMOTE_DIR="${REMOTE_DIR:-/workspace/run/sweep2}"
SSHOPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=15"
if [ -z "${REMOTE_HOSTS+x}" ]; then
  REMOTE_HOSTS=$(grep -v '^#' "$HERE/../../sshlist" 2>/dev/null | xargs || true)
fi
read -r -a HOSTS <<< "$REMOTE_HOSTS"

# ---- STG models, unchanged from the previous sweep (spec section 4) ----
LAT_PARAMS="--dmodel 1024 --dff 2048 --batch 1 --seq 256 --dvocal 2048 --head 8 --kvhead 2"
BW_PARAMS="--dmodel 8192 --dff 16384 --batch 16 --seq 2048 --dvocal 8192 --head 32 --kvhead 8"
# Full factorization palette — identical for both models (47 shapes).
SHAPES=$(python3 scripts/palette.py 4 8 16 64 128 256)

# ---- the 8 cells: name -> "bw_frac|size_weights|sizes|rho_target" ----
declare -A SPEC=(
  [lat_light_small]="0|$WEIGHTS_SMALL|$SIZES_SMALL|$RHO_LIGHT"
  [lat_light_large]="0|$WEIGHTS_LARGE|$SIZES_LARGE|$RHO_LIGHT"
  [lat_heavy_small]="0|$WEIGHTS_SMALL|$SIZES_SMALL|$RHO_HEAVY"
  [lat_heavy_large]="0|$WEIGHTS_LARGE|$SIZES_LARGE|$RHO_HEAVY"
  [bw_light_small]="1|$WEIGHTS_SMALL|$SIZES_SMALL|$RHO_LIGHT"
  [bw_light_large]="1|$WEIGHTS_LARGE|$SIZES_LARGE|$RHO_LIGHT"
  [bw_heavy_small]="1|$WEIGHTS_SMALL|$SIZES_SMALL|$RHO_HEAVY"
  [bw_heavy_large]="1|$WEIGHTS_LARGE|$SIZES_LARGE|$RHO_HEAVY"
)
# bw cells first so the slowest sims start earliest on distinct hosts.
ORDER=(bw_heavy_large bw_light_large bw_heavy_small bw_light_small \
       lat_heavy_large lat_light_large lat_heavy_small lat_light_small)

cell_rho()   { IFS='|' read -r _ _ _ r <<< "${SPEC[$1]}"; echo "$r"; }
cell_ia()    { awk -F, -v c="$1" '$1==c && $3!=""{ia=$3} END{print ia}' "$CAL/cells.csv"; }
host_for()   { local i=0; for n in "${ORDER[@]}"; do
                 [ "$n" = "$1" ] && { echo "${HOSTS[$((i % ${#HOSTS[@]}))]}"; return; }
                 i=$((i+1)); done; }

# Flat result-folder name for one run of this sweep (no failures):
# <placement>-<admission>-<bw|lat>-<heavy|light>-<small|large>-<block>-0.
# Block is the binary default 4x4x4 except folding == rfold pinned to 8x8x8.
flat_name() {  # <cell> <admission> <placement>
  local B=4x4x4; [ "$3" = "folding" ] && B=8x8x8
  echo "$3-$2-${1//_/-}-$B-0"
}

missing_combos() {  # <cell> -> lines "<admission> <placement>" lacking a local jobs.csv
  local name="$1" a p
  for a in "${ADMISSIONS[@]}"; do
    for p in "${POLICIES[@]}"; do
      [ -s "$RESULTS/$(flat_name "$name" "$a" "$p")/jobs.csv" ] || echo "$a $p"
    done
  done
}

do_plan() {
  local name n
  echo "[plan] missing admission x placement combos per cell:"
  for name in "${ORDER[@]}"; do
    n=$(missing_combos "$name" | wc -l)
    echo "  $name ($n): $(missing_combos "$name" | awk '{printf "%s/%s ", $1, $2}')"
  done
}

# ============================================================ lib (STG) ===
build_library() {
  if [ -f "$LIB/lat/1x1x4/chakra_trace.0.et" ] && [ -z "${REGEN_LIB:-}" ]; then
    echo "[lib] mixlib present — skipping (REGEN_LIB=1 to force)"; return
  fi
  echo "[lib] building STG library ($(echo "$SHAPES" | wc -l) shapes x 2 models)..."
  $DOCKER image inspect "$STG_IMAGE" >/dev/null 2>&1 || {
    echo "ERROR: docker image '$STG_IMAGE' not found (see astra-sim-artifacts)" >&2; exit 1; }
  rm -rf "$LIB"; mkdir -p "$LIB"
  local pal; pal=$(mktemp)
  for s in $SHAPES; do echo "lat $s"; done >  "$pal"
  for s in $SHAPES; do echo "bw $s";  done >> "$pal"
  if ! $DOCKER run --rm --ipc=host -v "$LIB:/work" -v "$pal:/pal.txt" \
    -e LATP="$LAT_PARAMS" -e BWP="$BW_PARAMS" "$STG_IMAGE" bash -c '
      cd /app/STG; mkdir -p ~/.parallel && touch ~/.parallel/will-cite
      PAR=$(( $(nproc) - 2 )); [ "$PAR" -lt 1 ] && PAR=1
      echo "[lib] STG worker pool: $PAR" >&2
      gen1(){ local m="$1" sh="$2"
        local dp=$(echo $sh|cut -dx -f1) tp=$(echo $sh|cut -dx -f2) pp=$(echo $sh|cut -dx -f3)
        local od="/work/$m/$sh"; mkdir -p "$od"
        local a; [ "$m" = lat ] && a="$LATP" || a="$BWP"
        if python3 main.py --output_dir "$od" --output_name chakra_trace --model_type dense \
             --dp $dp --tp $tp --pp $pp $a --num_stacks 8 --weight_sharded 0 \
             --chakra_schema_version v0.0.4 >/tmp/g_${m}_${sh}.log 2>&1; then echo "OK $m $sh"
        else echo "FAIL $m $sh: $(tail -1 /tmp/g_${m}_${sh}.log)"; fi; }
      while read m sh; do gen1 "$m" "$sh" &
        while [ $(jobs -r|wc -l) -ge "$PAR" ]; do wait -n; done; done < /pal.txt
      wait' | tee /tmp/libbuild.log | grep -E "^(OK|FAIL)" | \
    awk '{c[$1]++} END{print "[lib]  "c["OK"]+0" ok, "c["FAIL"]+0" failed"}'; then
    echo "ERROR: STG library docker pipeline failed" >&2; exit 1
  fi
  if grep -q '^FAIL' /tmp/libbuild.log; then
    echo "ERROR: STG failures (spec section 4: stop, do not shrink the palette):" >&2
    grep '^FAIL' /tmp/libbuild.log >&2; exit 1
  fi
  sudo chown -R "$(id -u):$(id -g)" "$LIB" 2>/dev/null || true
  for d in "$LIB"/lat/*/ "$LIB"/bw/*/; do
    [ -f "${d}chakra_trace.json" ] && mv -f "${d}chakra_trace.json" "${d}comm_group.json"; done
  local want nlat nbw nets
  want=$(echo "$SHAPES" | wc -l)
  nlat=$(ls "$LIB"/lat 2>/dev/null | wc -l); nbw=$(ls "$LIB"/bw 2>/dev/null | wc -l)
  nets=$(find "$LIB" -name 'chakra_trace.0.et' | wc -l)
  if [ "$nlat" -ne "$want" ] || [ "$nbw" -ne "$want" ] || [ "$nets" -ne $((2 * want)) ]; then
    echo "ERROR: incomplete mixlib: lat=$nlat bw=$nbw et=$nets (want $want/$want/$((2 * want)))" >&2
    exit 1
  fi
  echo "[lib] done: lat=$nlat bw=$nbw shapes, $nets trace sets"
}

# ============================================================= calibrate ===
do_calibrate() {
  [ -f "$BIN" ] || { echo "ERROR: build the binary first: $BIN" >&2; exit 1; }
  [ -f "$LIB/lat/1x1x4/chakra_trace.0.et" ] || { echo "ERROR: run '$0 lib' first" >&2; exit 1; }
  mkdir -p "$CAL"
  python3 scripts/calibrate.py measure --lib "$LIB" --out "$CAL/service_times.csv" \
    --maxpar "${MAXPAR_CAL:-$(( $(nproc) - 2 ))}"
}

# =================================================================== gen ===
gen_cell() {  # <name> <ia>
  local name="$1" ia="$2" bf sw sizes rho
  IFS='|' read -r bf sw sizes rho <<< "${SPEC[$name]}"
  python3 scripts/gen_mixed.py --out "$TRACES/$name" --lib "$LIB" --n "$N_JOBS" \
    --seed "$SEED" --bw-frac "$bf" --bw-max-size "$BW_MAX" \
    --size-weights "$sw" --mean-ia-ns "$ia" || return 1
  rm -rf "$TRACES/$name/output"
}

do_gen() {
  [ -f "$CAL/service_times.csv" ] || { echo "ERROR: run '$0 calibrate' first" >&2; exit 1; }
  mkdir -p "$TRACES"
  echo "cell,rho_target,mean_ia_ns,work_npu_ns,unit_span" > "$CAL/cells.csv"
  local name bf sw sizes rho ia W S1
  for name in "${ORDER[@]}"; do
    IFS='|' read -r bf sw sizes rho <<< "${SPEC[$name]}"
    read -r ia W S1 < <(python3 scripts/calibrate.py ia --lib "$LIB" \
      --svc "$CAL/service_times.csv" --bw-frac "$bf" --size-weights "$sw" \
      --rho "$rho" --n "$N_JOBS" --seed "$SEED" --bw-max-size "$BW_MAX")
    echo "==> $name: rho_target=$rho mean_ia=${ia}ns"
    gen_cell "$name" "$ia"
    echo "$name,$rho,$ia,$W,$S1" >> "$CAL/cells.csv"
    python3 scripts/verify_trace.py "$TRACES/$name" --svc "$CAL/service_times.csv" \
      --rho "$rho" --sizes "$sizes" || exit 1
  done
  echo "[gen] all cells generated + gated; ia table: $CAL/cells.csv"
}

# =================================================================== run ===
RSH() { local h="$1"; shift; ssh $SSHOPTS "$h" "$@"; }

setup_host() {
  local h="$1"
  RSH "$h" "mkdir -p $REMOTE_DIR/bin $REMOTE_DIR/cells" || return 1
  RSH "$h" "sudo apt-get install -y -q libprotobuf23 libscotch-6.1 libhwloc15 pigz >/dev/null 2>&1 || true"
  scp $SSHOPTS -q "$BIN" "$h:$REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable" || return 1
  if RSH "$h" "ldd $REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable 2>/dev/null | grep -q 'not found'"; then
    echo "[setup] $h: binary has unresolved libs:" >&2
    RSH "$h" "ldd $REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable | grep 'not found'" >&2
    return 1
  fi
  tar -C "$HERE" -chf - scripts configs | RSH "$h" "tar -C $REMOTE_DIR -xf -" || return 1
  tar -C "$HERE" -cf - mixlib | pigz -1 | RSH "$h" "cd $REMOTE_DIR && pigz -d | tar -xf -" || return 1
  RSH "$h" "test -f $REMOTE_DIR/mixlib/lat/1x1x4/chakra_trace.0.et" \
    && echo "[setup] $h ready" || { echo "[setup] $h FAILED" >&2; return 1; }
}

run_cell_on_host() {  # <name> <host> <ia>
  local name="$1" h="$2" ia="$3" bf sw sizes rho
  IFS='|' read -r bf sw sizes rho <<< "${SPEC[$name]}"
  local keep="${KEEPLOGS:-}"
  # Regenerate the cell remotely (traces are not shipped). Remote output is
  # scratch (nested <adm>/<plc> layout) — wipe it; LOCAL results/ is never
  # wiped (merge-don't-wipe).
  RSH "$h" "cd $REMOTE_DIR; ulimit -n 1048576
    python3 scripts/gen_mixed.py --out cells/$name --lib mixlib --n $N_JOBS --seed $SEED \
      --bw-frac $bf --bw-max-size $BW_MAX --size-weights '$sw' --mean-ia-ns $ia >/dev/null || exit 9
    rm -rf cells/$name/output" || {
      echo "ERROR: $name on $h: remote cell gen failed" >&2; return 1; }
  # arrival files must be byte-identical local vs remote (same seed + ia string)
  local lmd5 rmd5
  lmd5=$(md5sum "$TRACES/$name/arrivals.csv" | cut -d' ' -f1)
  rmd5=$(RSH "$h" "md5sum $REMOTE_DIR/cells/$name/arrivals.csv" | cut -d' ' -f1)
  [ "$lmd5" = "$rmd5" ] || { echo "ERROR: $name arrivals.csv local/remote mismatch" >&2; return 1; }
  # One admission batch at a time; its missing placements run in parallel
  # (<=7 concurrent sims, the same envelope as the original sweep).
  local a p plcs fail=0 dst
  for a in "${ADMISSIONS[@]}"; do
    plcs=$(missing_combos "$name" | awk -v a="$a" '$1==a{print $2}' | xargs)
    [ -z "$plcs" ] && continue
    echo "[run] $name on $h: $a x ( $plcs )"
    RSH "$h" "cd $REMOTE_DIR; ulimit -n 1048576
      pids=; bfail=0
      for p in $plcs; do ASTRA_SIM=$REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable \
        CFG=$REMOTE_DIR/configs bash scripts/run_policy.sh $REMOTE_DIR/cells/$name $a \$p & pids=\"\$pids \$!\"; done
      for pid in \$pids; do wait \$pid || bfail=1; done
      if [ \$bfail -eq 0 ] && [ -z \"$keep\" ]; then
        find cells/$name/output/$a -name '*.log' ! -name 'run.log' -delete 2>/dev/null || true
      fi
      exit \$bfail" || {
        echo "WARNING: $name on $h: >=1 $a sim failed (logs kept remotely)" >&2; fail=1; }
    # Merge results for exactly the combos just run; never touch other dirs.
    for p in $plcs; do
      dst="$RESULTS/$(flat_name "$name" "$a" "$p")"
      mkdir -p "$dst"
      rsync -az -e "ssh $SSHOPTS" "$h:$REMOTE_DIR/cells/$name/output/$a/$p/" \
        "$dst/" || fail=1
      # Per-combo sanity gate: results exist and summary names the right pair
      # (the folding arm runs the binary's rfold policy at whole-torus block).
      bp="$p"; [ "$p" = "folding" ] && bp="rfold"
      if [ ! -s "$dst/jobs.csv" ] ||
         ! grep -q "^admission_policy:[[:space:]]*$a\$" "$dst/summary.txt" ||
         ! grep -q "^placement_policy:[[:space:]]*$bp\$" "$dst/summary.txt"; then
        echo "ERROR: $name: bad or missing results for $a/$p" >&2; fail=1
      fi
    done
  done
  return "$fail"
}

run_cell_local() {  # <name>  (smoke tests only; needs <cell>/jobs/ regenerated via gen)
  local name="$1" a p plcs fail=0
  for a in "${ADMISSIONS[@]}"; do
    plcs=$(missing_combos "$name" | awk -v a="$a" '$1==a{print $2}' | xargs)
    [ -z "$plcs" ] && continue
    local pids=()
    for p in $plcs; do
      OUT="$RESULTS/$(flat_name "$name" "$a" "$p")" \
        ASTRA_SIM="$BIN" CFG="$CFG_DIR" bash scripts/run_policy.sh "$TRACES/$name" "$a" "$p" & pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || fail=1; done
  done
  [ "$fail" = 1 ] && echo "WARNING: a sim failed for $name" >&2
}

verify_loop() {
  local name rho out all_ok=1
  for name in "${ORDER[@]}"; do
    rho=$(cell_rho "$name")
    if out=$(python3 scripts/calibrate.py check "$TRACES/$name" \
        --results "$RESULTS/$(flat_name "$name" fifo firstfit)" --target "$rho" 2>&1); then
      echo "[verify] $name: $out"
    else
      echo "[verify] $name: $out" >&2
      echo "[verify] $name out of band or incomplete — surfacing; the admission" \
           "sweep never rescales (arrivals + fifo plane are frozen)" >&2
      all_ok=0
    fi
  done
  [ "$all_ok" = 1 ] && echo "[verify] all cells in band" || return 1
}

do_run() {
  [ -f "$CAL/cells.csv" ] || { echo "ERROR: run '$0 gen' first" >&2; exit 1; }
  do_plan
  if [ ${#HOSTS[@]} -eq 0 ]; then
    echo "[run] no REMOTE_HOSTS — running locally (slow)"
    local name; for name in "${ORDER[@]}"; do
      echo "==> $name (local)"; run_cell_local "$name"; done
  else
    echo "[run] setting up ${#HOSTS[@]} hosts in parallel"
    local pids=() h fail=0
    for h in "${HOSTS[@]}"; do setup_host "$h" & pids+=($!); done
    for p in "${pids[@]}"; do wait "$p" || fail=1; done
    [ "$fail" = 1 ] && { echo "ERROR: host setup failed" >&2; exit 1; }
    # per-host sequential queues, hosts in parallel (round-robin over ORDER)
    declare -A QUEUE=()
    local i=0 name
    for name in "${ORDER[@]}"; do
      h="${HOSTS[$((i % ${#HOSTS[@]}))]}"; QUEUE[$h]="${QUEUE[$h]:-} $name"; i=$((i+1))
    done
    pids=(); fail=0
    for h in "${HOSTS[@]}"; do
      ( qfail=0
        for name in ${QUEUE[$h]:-}; do
          echo "[run] $name on $h"
          run_cell_on_host "$name" "$h" "$(cell_ia "$name")" || qfail=1
        done
        exit "$qfail" ) & pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || fail=1; done
    [ "$fail" = 1 ] && echo "WARNING: some cells failed — verify will catch them" >&2
  fi
  verify_loop
}

# ================================================================== main ===
case "${1:-all}" in
  lib)       build_library ;;
  calibrate) do_calibrate ;;
  gen)       do_gen ;;
  plan)      do_plan ;;
  run)       do_run ;;
  all)       build_library; do_calibrate; do_gen; do_run ;;
  *) echo "usage: $0 {lib|calibrate|gen|plan|run|all}" >&2; exit 1 ;;
esac
