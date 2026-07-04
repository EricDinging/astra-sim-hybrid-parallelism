#!/bin/bash
# Regression test: synchronously-completing nodes must not strand a rank.
#
# Job 0's trace has a zero-tensor-size root COMP, which issue_comp() finishes
# synchronously (skip_invalid) without registering any event. Its children
# must still be issued in the same issue_dep_free_nodes call (fix-point
# re-scan). If they are not, job 0 -- which arrives first and is ~3x shorter
# than job 1 -- only completes after job 1 drains the event queue, so it
# finishes LAST and its JCT balloons to job 1's completion time.
set -e

SCRIPT_DIR=$(dirname "$(realpath $0)")
ASTRA_SIM_BIN=${ASTRA_SIM_BIN:-${SCRIPT_DIR}/../../build/astra_analytical/build/bin/AstraSim_Analytical_Reconfigurable}

rm -rf "${SCRIPT_DIR}/outputs"
mkdir -p "${SCRIPT_DIR}/outputs"

echo "[$0] Generating inputs..."
PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3 "${SCRIPT_DIR}/inputs/gen.py"

GEN=${SCRIPT_DIR}/outputs/gen
echo "[$0] Running ASTRA-sim..."
"${ASTRA_SIM_BIN}" \
    --system-configuration="${SCRIPT_DIR}/inputs/system.json" \
    --remote-memory-configuration="${SCRIPT_DIR}/inputs/remote_memory.json" \
    --network-configuration="${SCRIPT_DIR}/inputs/network.yml" \
    --bw-schedule="${GEN}/bandwidth_schedule.txt" \
    --latency-schedule="${GEN}/latency_schedule.txt" \
    --job-arrival-file="${GEN}/arrivals.csv" \
    --jobs-dir="${GEN}/jobs" \
    --placement-policy=firstfit \
    --admission-policy=fifo \
    --npus-per-dim=4,4,4 \
    --num-queues-per-dim=1 \
    --comm-scale=1.0 --injection-scale=1.0 --rendezvous-protocol=false \
    --logging-folder="${SCRIPT_DIR}/outputs" \
    > "${SCRIPT_DIR}/outputs/stdout.txt" 2>&1

echo "[$0] Checking completions..."
jct0=$(sed -n 's/.*job 0 COMPLETED at tick [0-9]* (jct=\([0-9]*\) ns).*/\1/p' \
    "${SCRIPT_DIR}/outputs/stdout.txt")
jct1=$(sed -n 's/.*job 1 COMPLETED at tick [0-9]* (jct=\([0-9]*\) ns).*/\1/p' \
    "${SCRIPT_DIR}/outputs/stdout.txt")
end0=$(sed -n 's/.*job 0 COMPLETED at tick \([0-9]*\).*/\1/p' \
    "${SCRIPT_DIR}/outputs/stdout.txt")
end1=$(sed -n 's/.*job 1 COMPLETED at tick \([0-9]*\).*/\1/p' \
    "${SCRIPT_DIR}/outputs/stdout.txt")

if [ -z "$jct0" ] || [ -z "$jct1" ]; then
    echo "[$0] Failed: missing COMPLETED lines (jct0='$jct0' jct1='$jct1')."
    exit 1
fi
# The strand candidate must finish strictly before the long job, and well
# under half its JCT. A stranded job 0 completes after job 1 instead.
if [ "$end0" -ge "$end1" ]; then
    echo "[$0] Failed: job 0 completed at $end0, after job 1 at $end1" \
         "-- rank was stranded by its synchronously-completed root."
    exit 1
fi
if [ $((jct0 * 2)) -ge "$jct1" ]; then
    echo "[$0] Failed: jct0=$jct0 not well under half of jct1=$jct1."
    exit 1
fi

echo "[$0] Ok. (jct0=${jct0}ns, jct1=${jct1}ns)"
