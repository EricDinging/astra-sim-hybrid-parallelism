#!/bin/bash
# Regression test: cross-rank ordinal divergence must abort at attach.
#
# Per-group ordered collective admission (and ordinal-derived stream
# ids/tags) require every member rank of a comm group to derive the same
# ordinal sequence of collectives. The divergent job below numbers its two
# ALL_REDUCEs oppositely on rank 1, so the ranks disagree on which
# collective is ordinal 0; without the fingerprint check the job wedges at
# runtime with no indication why. The aligned twin must run to completion.
set -e

SCRIPT_DIR=$(dirname "$(realpath $0)")
ASTRA_SIM_BIN=${ASTRA_SIM_BIN:-${SCRIPT_DIR}/../../build/astra_analytical/build/bin/AstraSim_Analytical_Reconfigurable}

rm -rf "${SCRIPT_DIR}/outputs"
mkdir -p "${SCRIPT_DIR}/outputs"

echo "[$0] Generating inputs..."
PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3 "${SCRIPT_DIR}/inputs/gen.py"

GEN=${SCRIPT_DIR}/outputs/gen

run_variant() {
    local variant=$1
    mkdir -p "${SCRIPT_DIR}/outputs/${variant}"
    "${ASTRA_SIM_BIN}" \
        --system-configuration="${SCRIPT_DIR}/inputs/system.json" \
        --remote-memory-configuration="${SCRIPT_DIR}/inputs/remote_memory.json" \
        --network-configuration="${SCRIPT_DIR}/inputs/network.yml" \
        --bw-schedule="${GEN}/bandwidth_schedule.txt" \
        --latency-schedule="${GEN}/latency_schedule.txt" \
        --job-arrival-file="${GEN}/arrivals.csv" \
        --jobs-dir="${GEN}/jobs_${variant}" \
        --placement-policy=firstfit \
        --admission-policy=fifo \
        --npus-per-dim=4,4,4 \
        --num-queues-per-dim=1 \
        --comm-scale=1.0 --injection-scale=1.0 --rendezvous-protocol=false \
        --logging-folder="${SCRIPT_DIR}/outputs/${variant}" \
        > "${SCRIPT_DIR}/outputs/${variant}/stdout.txt" 2>&1
}

echo "[$0] Running aligned variant (must complete)..."
if ! run_variant aligned; then
    echo "[$0] Failed: aligned variant did not exit cleanly."
    exit 1
fi
if ! grep -q "job 0 COMPLETED" "${SCRIPT_DIR}/outputs/aligned/stdout.txt"; then
    echo "[$0] Failed: aligned variant did not complete job 0."
    exit 1
fi

echo "[$0] Running divergent variant (must abort with a diagnostic)..."
if run_variant divergent; then
    echo "[$0] Failed: divergent variant exited 0; expected a fingerprint" \
         "mismatch abort."
    exit 1
fi
if ! grep -q "ordinal sequence diverges" \
        "${SCRIPT_DIR}/outputs/divergent/stdout.txt"; then
    echo "[$0] Failed: divergent variant died without the fingerprint" \
         "diagnostic."
    exit 1
fi

echo "[$0] Ok."
