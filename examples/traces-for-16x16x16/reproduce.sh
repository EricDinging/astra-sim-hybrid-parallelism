#!/bin/bash
# reproduce.sh — Pareto-sized, load-calibrated 16x16x16 placement sweep.
#
# Spec: docs/superpowers/specs/2026-06-05-16x16x16-pareto-sweep-design.md
# Phases (positional arg, default "all"):
#   configs    generate network.yml + 4096x4096 BW/LT schedule matrices
#   lib        kvhead probe + stage trace library via Docker; palette.json
#   calibrate  isolated service time per lib entry -> service_times.csv
#              (farm worker pool; local when SMOKE=1 / no hosts)
#   gen        exact per-cell mean-ia from rho targets; generate cells; gates
#   probe      firstfit probe per cell on the farm (10-job prefix, or the
#              full trace where SPEC's probe_jobs says so), +/-0.10 band,
#              rescale ia + regen + re-probe (<=3 iterations) until in band
#   run        MISSING policy x cell combos through the worker pool
#              (skip-existing = idempotent); measured rho reported, never
#              auto-rerun (spec section 7)
#
# Results land in flat per-run folders (see results/README.md):
#   results/<placement>-fifo-<bw|lat>-<heavy|light>-<small|large>-<block>-0/
#
# Env:
#   REMOTE_HOSTS  user@host list (default: repo-root sshlist); "" = local
#   MAXPAR_HOST   sim slots per pool host        (default 2, spec section 8)
#   SMOKE=1       miniaturized local pipeline (smoke_lat_small, 12 jobs)
#   STAGE_IMAGE     Docker image bundling stage      (default astra:latest)
#   DOCKER        docker command                 (default "sudo docker")
#   REMOTE_DIR    remote working dir             (default /workspace/run/sweep16)
#   MAXPAR_CAL    parallel local calibration sims (default nproc-2; SMOKE/local)
#   KEEPLOGS      1 = keep per-policy *.log files on the farm copies
#   REGEN_LIB     1 = force mixlib rebuild
# NOTE: `probe` appends rescaled-ia rows to calibration/cells.csv (last row
# wins); rerunning `gen` truncates cells.csv and so resets those rescales.
set -uo pipefail
HERE=$(dirname "$(realpath "$0")")
cd "$HERE"

LIB="$HERE/mixlib"
TRACES="$HERE/traces"
RESULTS="$HERE/results"
PROBEDIR="$HERE/probe-traces"
CAL="$HERE/calibration"
CFG_DIR="$HERE/configs"
FARMLOG="$HERE/farm-logs"
BIN="$HERE/../../build/astra_analytical/build/bin/AstraSim_Analytical_Reconfigurable"
POLICIES=(firstfit random sfc l1clustering topomatch folding rfold)
ADMISSIONS=(fifo sjdf sjsf swf easy ljdf ljsf ljsfpack lwf)
# sweep planes beyond the placement x admission grid (8x8x8 conventions):
# rfold block sweep (fifo, no failures) and NPU-failure sweep (7 placements
# x {fifo,easy} on one bw-heavy and one lat-light cell).
BLOCKS=(1x1x1 2x2x2 8x8x8)        # 4x4x4 = binary default, covered by the grid
FAILPROBS=(0.001 0.002 0.005 0.01)
FAIL_CELLS=(bw_heavy_large lat_light_small)
FAIL_ADMISSIONS=(fifo easy)
N_JOBS=120; PROBE_JOBS=10; SEED=1
RHO_LIGHT=0.15; RHO_HEAVY=0.85
ONF_SMALL=0.05; ONF_LARGE=0
SIZES_SMALL=$(python3 scripts/palette.py sizes small | paste -sd,)
SIZES_LARGE=$(python3 scripts/palette.py sizes large | paste -sd,)
# Tweaked heavy_large mixes (2026-06-10): curated subsets of the large grid
# that keep the cells queue-bound but make whole-torus folding hit
# fragmentation defers that block-level rfold absorbs via OCS scatter.
# Bulk fragmenters (512-672, awkward 10/12/14-dim shapes) shred contiguity;
# victims (1024/1536, all-{8,12,16}-dim shapes) are exactly the sizes whose
# ring closures land on 4x4x4 block faces, i.e. the only OCS-scatterable
# footprints. Result: rfold beats folding by 21-26% p50 JCT (vs ~2% on the
# uncurated grid) while every other policy ordering is preserved.
SIZES_HL_BW="512,576,640,1024,1536"
SIZES_HL_LAT="576,640,672,1024,1536"
STAGE_IMAGE="${STAGE_IMAGE:-astra:latest}"
DOCKER="${DOCKER:-sudo docker}"
REMOTE_DIR="${REMOTE_DIR:-/workspace/run/sweep16}"
MAXPAR_HOST="${MAXPAR_HOST:-2}"
SSHOPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=15"

# ---- stage models (spec 4.3). lat kvhead is finalized by the kvhead probe.
BW_PARAMS="--dmodel 8192 --dff 16384 --batch 16 --seq 2048 --dvocal 8192 --head 32 --kvhead 8"
lat_params() {  # <kvhead>
  echo "--dmodel 1024 --dff 2048 --batch 1 --seq 256 --dvocal 2048 --head 16 --kvhead $1"
}
NUM_STACKS=16

# ---- cells: name -> "bw_frac|sizes|one_node_frac|rho[|probe_jobs]" ------
# (longest first). probe_jobs defaults to PROBE_JOBS (10); lat_heavy_large
# probes on the FULL trace: its victim sizes sit mid-sequence, so the 10-job
# prefix under-measures offered load by ~35% and the rescale overheats the
# cell to measured rho ~1.3, where capacity-blocking drowns the placement
# differential the cell exists to expose. bw_heavy_large keeps the 10-job
# probe (its prefix is representative; the standard flow froze its ia).
if [ -n "${SMOKE:-}" ]; then
  POLICIES=(firstfit rfold)
  N_JOBS=12
  # full isolation: a smoke run must never clobber the real mixlib /
  # calibration / probe artifacts (all of smoke-traces/ is gitignored)
  TRACES="$HERE/smoke-traces/traces"
  RESULTS="$HERE/smoke-traces/results"
  LIB="$HERE/smoke-traces/mixlib"
  CAL="$HERE/smoke-traces/calibration"
  PROBEDIR="$HERE/smoke-traces/probe"
  FARMLOG="$HERE/smoke-traces/farm-logs"
  REMOTE_HOSTS=""
  CELLS=(smoke_lat_small)
  declare -A SPEC=([smoke_lat_small]="0|8,16|$ONF_SMALL|$RHO_LIGHT")
else
  CELLS=(bw_heavy_large bw_light_large bw_heavy_small bw_light_small
         lat_heavy_large lat_light_large lat_heavy_small lat_light_small)
  declare -A SPEC=(
    [bw_heavy_large]="1|$SIZES_HL_BW|$ONF_LARGE|$RHO_HEAVY"
    [bw_light_large]="1|$SIZES_LARGE|$ONF_LARGE|$RHO_LIGHT"
    [bw_heavy_small]="1|$SIZES_SMALL|$ONF_SMALL|$RHO_HEAVY"
    [bw_light_small]="1|$SIZES_SMALL|$ONF_SMALL|$RHO_LIGHT"
    [lat_heavy_large]="0|$SIZES_HL_LAT|$ONF_LARGE|$RHO_HEAVY|$N_JOBS"
    [lat_light_large]="0|$SIZES_LARGE|$ONF_LARGE|$RHO_LIGHT"
    [lat_heavy_small]="0|$SIZES_SMALL|$ONF_SMALL|$RHO_HEAVY"
    [lat_light_small]="0|$SIZES_SMALL|$ONF_SMALL|$RHO_LIGHT"
  )
fi
if [ -z "${REMOTE_HOSTS+x}" ]; then
  REMOTE_HOSTS=$(grep -v '^#' "$HERE/../../sshlist" 2>/dev/null | xargs || true)
fi
read -r -a HOSTS <<< "$REMOTE_HOSTS"

cell_field() { local n="$1" i="$2"; IFS='|' read -r -a F <<< "${SPEC[$n]}"; echo "${F[$i]}"; }
cell_rho()   { cell_field "$1" 3; }
cell_ia()    { awk -F, -v c="$1" '$1==c{ia=$3} END{print ia}' "$CAL/cells.csv"; }

# ============================================================== configs ===
do_configs() {
  python3 scripts/gen_configs.py --out "$CFG_DIR" || exit 1
  python3 scripts/gen_configs.py --out "$CFG_DIR" --check || exit 1
}

# ================================================================== lib ===
probe_kvhead() {  # decide lat kvhead: smallest of 4,8,16 stage accepts for tp=16
  if [ -f "$CAL/lat_kvhead.txt" ]; then cat "$CAL/lat_kvhead.txt"; return; fi
  local kv d
  for kv in 4 8 16; do
    d=$(mktemp -d)
    if $DOCKER run --rm --ipc=host -v "$d:/work" "$STAGE_IMAGE" bash -c \
      "cd /app/stage && python3 main.py --output_dir /work --output_name chakra_trace \
       --model_type dense --dp 2 --tp 16 --pp 2 $(lat_params "$kv") \
       --num_stacks $NUM_STACKS --weight_sharded 0 \
       --chakra_schema_version v0.0.4" >/dev/null 2>&1 \
       && [ -n "$(find "$d" -name 'chakra_trace.0.et' 2>/dev/null)" ]; then
      sudo rm -rf "$d"; mkdir -p "$CAL"; echo "$kv" | tee "$CAL/lat_kvhead.txt"; return
    fi
    sudo rm -rf "$d"
  done
  echo "ERROR: no kvhead in {4,8,16} lets the lat model build tp=16" >&2
  exit 1
}

build_library() {
  if [ -f "$LIB/lat/1x1x1/chakra_trace.0.et" ] && [ -z "${REGEN_LIB:-}" ]; then
    echo "[lib] mixlib present — skipping (REGEN_LIB=1 to force)"; return
  fi
  $DOCKER image inspect "$STAGE_IMAGE" >/dev/null 2>&1 || {
    echo "ERROR: docker image '$STAGE_IMAGE' not found" >&2; exit 1; }
  local kv pal
  if [ -n "${SMOKE:-}" ]; then
    kv=4  # smoke palette has no tp=16 shape; probe unnecessary
    pal=$(mktemp)
    for s in 1 8 16; do
      python3 scripts/palette.py shapes "$s" | sed 's/^/lat /'; done > "$pal"
  else
    kv=$(probe_kvhead)
    echo "[lib] lat kvhead = $kv"
    pal=$(mktemp)
    { python3 scripts/palette.py shapes small; python3 scripts/palette.py shapes large;
      echo 1x1x1; } | sed 's/^/lat /'  >  "$pal"
    { python3 scripts/palette.py shapes small; python3 scripts/palette.py shapes large;
      echo 1x1x1; } | sed 's/^/bw /'   >> "$pal"
  fi
  echo "[lib] building stage library ($(wc -l < "$pal") entries)..."
  rm -rf "$LIB"; mkdir -p "$LIB"
  $DOCKER run --rm --ipc=host -v "$LIB:/work" -v "$pal:/pal.txt" \
    -e LATP="$(lat_params "$kv")" -e BWP="$BW_PARAMS" -e STACKS="$NUM_STACKS" \
    "$STAGE_IMAGE" bash -c '
      cd /app/stage; mkdir -p ~/.parallel && touch ~/.parallel/will-cite
      PAR=$(( $(nproc) - 2 )); [ "$PAR" -lt 1 ] && PAR=1
      echo "[lib] stage worker pool: $PAR" >&2
      gen1(){ local m="$1" sh="$2"
        local dp=$(echo $sh|cut -dx -f1) tp=$(echo $sh|cut -dx -f2) pp=$(echo $sh|cut -dx -f3)
        local od="/work/$m/$sh"; mkdir -p "$od"
        local a; [ "$m" = lat ] && a="$LATP" || a="$BWP"
        if python3 main.py --output_dir "$od" --output_name chakra_trace --model_type dense \
             --dp $dp --tp $tp --pp $pp $a --num_stacks $STACKS --weight_sharded 0 \
             --chakra_schema_version v0.0.4 >/tmp/g_${m}_${sh}.log 2>&1 \
           && [ -f "$od/chakra_trace.0.et" ]; then echo "OK $m $sh"
        else echo "FAIL $m $sh: $(tail -1 /tmp/g_${m}_${sh}.log)"; rm -rf "$od"; fi; }
      while read m sh; do gen1 "$m" "$sh" &
        while [ $(jobs -r|wc -l) -ge "$PAR" ]; do wait -n; done; done < /pal.txt
      wait' | tee /tmp/libbuild16.log | grep -cE "^OK" \
    | xargs -I{} echo "[lib] {} entries built"
  sudo chown -R "$(id -u):$(id -g)" "$LIB" 2>/dev/null || true
  for d in "$LIB"/lat/*/ "$LIB"/bw/*/; do
    [ -f "${d}chakra_trace.json" ] && mv -f "${d}chakra_trace.json" "${d}comm_group.json"
  done
  mkdir -p "$CAL"
  # per-shape FAILs are allowed; build_palette_json hard-fails only if a
  # size loses ALL its orderings in either model (spec 4.2)
  if [ -n "${SMOKE:-}" ]; then
    grep -E '^FAIL' /tmp/libbuild16.log && { echo "ERROR: smoke lib FAIL" >&2; exit 1; }
    echo '{"smoke": true}' > "$CAL/palette.json"
  else
    python3 scripts/build_palette_json.py --log /tmp/libbuild16.log \
      --out "$CAL/palette.json" --lat-kvhead "$kv" || exit 1
  fi
}

# ============================================================ calibrate ===
setup_host() {
  local h="${1%%=*}"  # strip a per-host slot suffix (user@host=N)
  ssh $SSHOPTS "$h" "mkdir -p $REMOTE_DIR/bin $REMOTE_DIR/cells" || return 1
  ssh $SSHOPTS "$h" "sudo apt-get install -y -q libprotobuf23 libscotch-6.1 libhwloc15 pigz >/dev/null 2>&1 || true"
  scp $SSHOPTS -q "$BIN" "$h:$REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable" || return 1
  if ssh $SSHOPTS "$h" "ldd $REMOTE_DIR/bin/AstraSim_Analytical_Reconfigurable 2>/dev/null | grep -q 'not found'"; then
    echo "[setup] $h: binary has unresolved libs" >&2; return 1
  fi
  tar -C "$HERE" -chf - scripts configs | ssh $SSHOPTS "$h" "tar -C $REMOTE_DIR -xf -" || return 1
  if ! ssh $SSHOPTS "$h" "test -f $REMOTE_DIR/mixlib/lat/1x1x1/chakra_trace.0.et" \
     || [ -n "${FORCE_SHIP_LIB:-}" ]; then
    tar -C "$HERE" -cf - mixlib | pigz -1 | ssh $SSHOPTS "$h" \
      "cd $REMOTE_DIR && rm -rf mixlib && pigz -d | tar -xf -" || return 1
  fi
  ssh $SSHOPTS "$h" "test -f $REMOTE_DIR/mixlib/lat/1x1x1/chakra_trace.0.et" \
    && echo "[setup] $h ready" || { echo "[setup] $h FAILED" >&2; return 1; }
}

setup_hosts() {
  local pids=() h fail=0
  echo "[setup] preparing ${#HOSTS[@]} hosts"
  for h in "${HOSTS[@]}"; do setup_host "$h" & pids+=($!); done
  for p in "${pids[@]}"; do wait "$p" || fail=1; done
  [ "$fail" = 1 ] && { echo "ERROR: host setup failed" >&2; exit 1; }
}

do_calibrate() {
  [ -f "$BIN" ] || { echo "ERROR: build the binary first: $BIN" >&2; exit 1; }
  [ -f "$LIB/lat/1x1x1/chakra_trace.0.et" ] || { echo "ERROR: run '$0 lib' first" >&2; exit 1; }
  mkdir -p "$CAL"
  if [ ${#HOSTS[@]} -eq 0 ]; then
    python3 scripts/calibrate.py measure --lib "$LIB" --out "$CAL/service_times.csv" \
      --maxpar "${MAXPAR_CAL:-$(( $(nproc) - 2 ))}"
    return
  fi
  setup_hosts
  mkdir -p "$CAL/shards" "$FARMLOG/cal"
  # items sorted by size DESC: the multi-hour bw 2048-rank sims start first
  python3 - "$LIB" <<'EOF' > "$FARMLOG/cal/items.txt"
import sys
sys.path.insert(0, "scripts")
from calibrate import lib_entries
for model, shape, size, _ in sorted(lib_entries(sys.argv[1]),
                                    key=lambda e: -e[2]):
    print(model, shape, size)
EOF
  python3 scripts/pool.py --hosts "$REMOTE_HOSTS" --slots "$MAXPAR_HOST" \
    --items "$FARMLOG/cal/items.txt" --handler scripts/farm_cal_item.sh \
    --log-dir "$FARMLOG/cal" || {
      echo "ERROR: calibration items failed — rerun '$0 calibrate' (idempotent)" >&2
      exit 1; }
  python3 scripts/calibrate.py merge --lib "$LIB" --shards "$CAL/shards" \
    --out "$CAL/service_times.csv"
}

# ================================================================== gen ===
gen_cell() {  # <name> <ia> <n> <outbase>
  local name="$1" ia="$2" n="$3" base="$4" bf sizes onf rho pj
  IFS='|' read -r bf sizes onf rho pj <<< "${SPEC[$name]}"
  python3 scripts/gen_mixed.py --out "$base/$name" --lib "$LIB" --n "$n" \
    --seed "$SEED" --bw-frac "$bf" --sizes "$sizes" --one-node-frac "$onf" \
    --mean-ia-ns "$ia" || return 1
  rm -rf "$base/$name/output"
}

verify_cell() {  # <name> <outbase>
  local name="$1" base="$2" bf sizes onf rho pj
  IFS='|' read -r bf sizes onf rho pj <<< "${SPEC[$name]}"
  python3 scripts/verify_trace.py "$base/$name" --svc "$CAL/service_times.csv" \
    --lib "$LIB" --rho "$rho" --sizes "$sizes" --one-node-frac "$onf"
}

do_gen() {
  [ -f "$CAL/service_times.csv" ] || { echo "ERROR: run '$0 calibrate' first" >&2; exit 1; }
  mkdir -p "$TRACES"
  echo "cell,rho_target,mean_ia_ns,work_npu_ns,unit_span" > "$CAL/cells.csv"
  local name bf sizes onf rho pj ia W S1
  for name in "${CELLS[@]}"; do
    IFS='|' read -r bf sizes onf rho pj <<< "${SPEC[$name]}"
    read -r ia W S1 < <(python3 scripts/calibrate.py ia --lib "$LIB" \
      --svc "$CAL/service_times.csv" --bw-frac "$bf" --sizes "$sizes" \
      --one-node-frac "$onf" --rho "$rho" --n "$N_JOBS" --seed "$SEED")
    echo "==> $name: rho_target=$rho mean_ia=${ia}ns"
    gen_cell "$name" "$ia" "$N_JOBS" "$TRACES" || exit 1
    echo "$name,$rho,$ia,$W,$S1" >> "$CAL/cells.csv"
    verify_cell "$name" "$TRACES" || exit 1
  done
  echo "[gen] all cells generated + gated; ia table: $CAL/cells.csv"
}

# ================================================================ probe ===
probe_items() {  # regenerate local probe refs + items file for given cells
  local cells=("$@") name ia bf sizes onf rho pj
  : > "$FARMLOG/probe/items.txt"
  for name in "${cells[@]}"; do
    ia=$(cell_ia "$name")
    IFS='|' read -r bf sizes onf rho pj <<< "${SPEC[$name]}"
    pj="${pj:-$PROBE_JOBS}"
    rm -rf "$PROBEDIR/probe_$name"
    python3 scripts/gen_mixed.py --out "$PROBEDIR/probe_$name" --lib "$LIB" \
      --n "$pj" --seed "$SEED" --bw-frac "$bf" --sizes "$sizes" \
      --one-node-frac "$onf" --mean-ia-ns "$ia" >/dev/null || exit 1
    rm -rf "$PROBEDIR/probe_$name/output"
    echo "probe_$name fifo firstfit 4x4x4 0 $bf $sizes $onf $ia $pj" \
      >> "$FARMLOG/probe/items.txt"
  done
}

do_probe() {
  [ -f "$CAL/cells.csv" ] || { echo "ERROR: run '$0 gen' first" >&2; exit 1; }
  # probes are cheap — always start the phase from scratch. (probe_items
  # wipes local probe refs, so a stale done.txt skipping their sims would
  # leave check with no output to read.)
  rm -rf "$FARMLOG/probe"
  mkdir -p "$PROBEDIR" "$FARMLOG/probe"
  if [ ${#HOSTS[@]} -eq 0 ]; then  # SMOKE / local: run probes in-place
    local name
    for name in "${CELLS[@]}"; do
      probe_items "$name"
      OUT="$PROBEDIR/probe_$name/output/firstfit" ASTRA_SIM="$BIN" CFG="$CFG_DIR" \
        bash scripts/run_policy.sh "$PROBEDIR/probe_$name" fifo firstfit || exit 1
      python3 scripts/calibrate.py check "$PROBEDIR/probe_$name" \
        --target "$(cell_rho "$name")" --band 0.10 \
        || echo "[probe] $name out of band (local mode: surfaced only)"
    done
    return
  fi
  setup_hosts
  local pending=("${CELLS[@]}") iter=0 name out new_ia next
  while [ ${#pending[@]} -gt 0 ]; do
    iter=$((iter + 1))
    if [ "$iter" -gt 3 ]; then
      echo "ERROR: cells still out of band after 3 probe iterations: ${pending[*]}" >&2
      exit 1
    fi
    echo "[probe] iteration $iter: ${pending[*]}"
    probe_items "${pending[@]}"
    LOCAL_BASE=probe-traces python3 scripts/pool.py --hosts "$REMOTE_HOSTS" \
      --slots "$MAXPAR_HOST" --items "$FARMLOG/probe/items.txt" \
      --handler scripts/farm_run_item.sh --log-dir "$FARMLOG/probe" || {
        echo "ERROR: probe sims failed — see $FARMLOG/probe" >&2; exit 1; }
    next=()
    for name in "${pending[@]}"; do
      out=$(python3 scripts/calibrate.py check "$PROBEDIR/probe_$name" \
        --target "$(cell_rho "$name")" --band 0.10) && {
          echo "[probe] $name: $out"; continue; }
      echo "[probe] $name: $out"
      new_ia=$(sed -n 's/.*suggested_ia=//p' <<< "$out")
      [ -z "$new_ia" ] && { echo "ERROR: $name probe check broken" >&2; exit 1; }
      echo "$name,$(cell_rho "$name"),$new_ia,," >> "$CAL/cells.csv"
      # no verify_cell here: a rescale deliberately moves rho_isolated off
      # the target so that MEASURED rho lands on it — the isolated-rho gate
      # only applies to the initial gen (8x8x8 behavior).
      gen_cell "$name" "$new_ia" "$N_JOBS" "$TRACES" || exit 1
      next+=("$name")
    done
    pending=("${next[@]:-}")
    [ -z "${pending[0]:-}" ] && pending=()
  done
  echo "[probe] all cells in band at the frozen ia"
}

# ================================================================== run ===
# flat results folder name, 8x8x8 convention (results/README.md):
#   <placement>-<admission>-<bw|lat>-<heavy|light>-<small|large>-<block>-<failprob>
# folding pins the whole-torus block.
flat_name() {  # <cell> <admission> <placement> [block] [failprob]
  local blk="${4:-4x4x4}"
  [ "$3" = folding ] && blk=16x16x16
  echo "$3-$2-$(echo "$1" | tr _ -)-$blk-${5:-0}"
}

# emit one item line per MISSING combo (skip-existing = idempotent reruns).
# item: <cell> <admission> <placement> <block> <failprob> <bf> <sizes> <onf> <ia> <n>
emit_item() {  # <cell> <admission> <placement> <block> <failprob>
  [ -s "$RESULTS/$(flat_name "$1" "$2" "$3" "$4" "$5")/jobs.csv" ] && return
  local bf sizes onf rho pj
  IFS='|' read -r bf sizes onf rho pj <<< "${SPEC[$1]}"
  echo "$1 $2 $3 $4 $5 $bf $sizes $onf $(cell_ia "$1") $N_JOBS"
}

sweep_items() {  # the full sweep, longest work first (heavy cells lead)
  local name a p b f
  for name in "${CELLS[@]}"; do       # CELLS is already longest-first
    # placement x admission grid at the default block, no failures
    for a in "${ADMISSIONS[@]}"; do
      for p in "${POLICIES[@]}"; do emit_item "$name" "$a" "$p" 4x4x4 0; done
    done
    # rfold block sweep (fifo)
    for b in "${BLOCKS[@]}"; do emit_item "$name" fifo rfold "$b" 0; done
  done
  # NPU-failure sweep
  for name in "${FAIL_CELLS[@]}"; do
    for f in "${FAILPROBS[@]}"; do
      for a in "${FAIL_ADMISSIONS[@]}"; do
        for p in "${POLICIES[@]}"; do emit_item "$name" "$a" "$p" 4x4x4 "$f"; done
      done
    done
  done
}

do_run() {
  [ -f "$CAL/cells.csv" ] || { echo "ERROR: run '$0 gen' first" >&2; exit 1; }
  local name p ia bf sizes onf rho pj
  mkdir -p "$RESULTS"
  if [ ${#HOSTS[@]} -eq 0 ]; then
    for name in "${CELLS[@]}"; do
      echo "==> $name (local, fifo plane only)"
      local pids=() fail=0
      for p in "${POLICIES[@]}"; do
        OUT="$RESULTS/$(flat_name "$name" fifo "$p")" ASTRA_SIM="$BIN" CFG="$CFG_DIR" \
          bash scripts/run_policy.sh "$TRACES/$name" fifo "$p" & pids+=($!)
      done
      for p in "${pids[@]}"; do wait "$p" || fail=1; done
      [ "$fail" = 1 ] && echo "WARNING: a policy sim failed for $name" >&2
    done
  else
    setup_hosts
    mkdir -p "$FARMLOG/run"
    sweep_items > "$FARMLOG/run/items.txt"
    echo "[run] $(wc -l < "$FARMLOG/run/items.txt") missing combos"
    RESULTS_BASE="$RESULTS" LOCAL_BASE=traces \
      python3 scripts/pool.py --hosts "$REMOTE_HOSTS" --slots "$MAXPAR_HOST" \
      --items "$FARMLOG/run/items.txt" --handler scripts/farm_run_item.sh \
      --log-dir "$FARMLOG/run" || \
      echo "WARNING: some run items failed — rerun '$0 run' (idempotent)" >&2
  fi
  # measured-rho summary: REPORT ONLY, no rescue loop (spec section 7)
  for name in "${CELLS[@]}"; do
    python3 scripts/calibrate.py check "$TRACES/$name" \
      --results "$RESULTS/$(flat_name "$name" fifo firstfit)" \
      --target "$(cell_rho "$name")" --band 0.05 \
      | sed "s/^/[run] $name measured: /" || true
  done
}

# ================================================================= main ===
case "${1:-all}" in
  configs)   do_configs ;;
  lib)       build_library ;;
  calibrate) do_calibrate ;;
  gen)       do_gen ;;
  probe)     do_probe ;;
  plan)      sweep_items | awk '{print $1, $2, $3, $4, $5}'; \
             echo "missing combos: $(sweep_items | wc -l)" >&2 ;;
  run)       do_run ;;
  all)       do_configs; build_library; do_calibrate; do_gen; do_probe; do_run ;;
  *) echo "usage: $0 {configs|lib|calibrate|gen|probe|plan|run|all}" >&2; exit 1 ;;
esac
