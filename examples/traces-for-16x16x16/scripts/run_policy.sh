#!/bin/bash
# Run one admission x placement combo on a candidate trace dir.
# Usage: run_policy.sh <candidate_dir> <admission> <placement>
# Reads:  <candidate_dir>/arrivals.csv, <candidate_dir>/jobs/
# Writes: <candidate_dir>/output/<admission>/<placement>/{jobs.csv,summary.txt,node_jobs.csv,*.log}
#         (or $OUT when set — used by local runs to write a flat results/ dir
#         directly; remote farm runs keep the nested scratch layout and the
#         driver rsyncs each combo into its flat results/ folder)
set -e
CAND="$1"
ADMISSION="$2"
POLICY="$3"
if [ -z "$CAND" ] || [ -z "$ADMISSION" ] || [ -z "$POLICY" ]; then
    echo "usage: run_policy.sh <candidate_dir> <admission> <placement>" >&2
    exit 1
fi
SCRIPT_DIR=$(dirname "$(realpath "$0")")
SWEEP_DIR="${SCRIPT_DIR}/.."
PROJECT_DIR="${SWEEP_DIR}/../.."
# ASTRA_SIM and CFG may be overridden via the environment (used for remote runs).
ASTRA_SIM="${ASTRA_SIM:-${PROJECT_DIR}/build/astra_analytical/build/bin/AstraSim_Analytical_Reconfigurable}"
CFG="${CFG:-${SWEEP_DIR}/configs}"

ulimit -n 1048576

# folding = rfold pinned to a whole-torus block: the unified replacement for
# the old standalone "folding" policy (same placement semantics; OCS is
# structurally impossible at whole-torus block size). Results keep the
# historical "folding" directory name -- folding IS rfold without reconfig.
BINPOLICY="$POLICY"
if [ "$POLICY" = "folding" ]; then
    BINPOLICY="rfold"
    # folding MEANS whole-torus block; a stray BLOCK from a knob-sweep
    # session would silently mislabel a custom-block rfold run as folding.
    if [ -n "${BLOCK:-}" ] && [ "$BLOCK" != "16x16x16" ]; then
        echo "ERROR: folding pins --block-size to the whole torus" \
             "(got BLOCK=$BLOCK); sweep blocks with POLICY=rfold" >&2
        exit 1
    fi
    BLOCK="16x16x16"
fi

# LABEL names the output subdir (defaults to <admission>/<placement>) so a knob
# sweep can place many runs of the same combo under distinct dirs; OUT
# overrides the destination entirely.
OUT="${OUT:-${CAND}/output/${LABEL:-${ADMISSION}/${POLICY}}}"
mkdir -p "${OUT}"

# Optional rfold knobs, passed only when set so the binary falls back to
# its own defaults otherwise (keeps the no-env invocation byte-identical).
EXTRA=()
[ -n "${BLOCK:-}" ] && EXTRA+=(--block-size="${BLOCK}")
[ -n "${SELECTOR:-}" ] && EXTRA+=(--rfold-selector="${SELECTOR}")
[ -n "${BUDGET:-}" ] && EXTRA+=(--rfold-search-budget="${BUDGET}")
[ -n "${METRIC:-}" ] && EXTRA+=(--defrag-metric="${METRIC}")
[ -n "${FAILURE_PROB:-}" ] && EXTRA+=(--failure-prob="${FAILURE_PROB}")

"${ASTRA_SIM}" \
    --system-configuration="${CFG}/system.json" \
    --remote-memory-configuration="${CFG}/remote_memory.json" \
    --network-configuration="${CFG}/network.yml" \
    --bw-schedule="${CFG}/bandwidth_schedule.txt" \
    --latency-schedule="${CFG}/latency_schedule.txt" \
    --logging-folder="${OUT}" \
    --num-queues-per-dim=1 \
    --comm-scale=1.0 \
    --injection-scale=1.0 \
    --rendezvous-protocol=false \
    --npus-per-dim="16,16,16" \
    --job-arrival-file="${CAND}/arrivals.csv" \
    --jobs-dir="${CAND}/jobs" \
    --placement-policy="${BINPOLICY}" \
    --admission-policy="${ADMISSION}" \
    "${EXTRA[@]}" \
    > "${OUT}/run.log" 2>&1
