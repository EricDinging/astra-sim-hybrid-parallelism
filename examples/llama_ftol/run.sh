#!/bin/bash

# run it in docker environment
set -e

# Parallelism flags (overridden by -t/-d/-p/-m)
TP=4
DP=1
PP=1
GPU_MEMORY=80  # GB

# Parse parallelism arguments
while getopts "t:d:p:m:" opt; do
    case ${opt} in
        t ) TP=$OPTARG ;;
        d ) DP=$OPTARG ;;
        p ) PP=$OPTARG ;;
        m ) GPU_MEMORY=$OPTARG ;;
        * ) echo "Usage: $0 [-t tp] [-d dp] [-p pp] [-m gpu_memory]";
                exit 1 ;;
    esac
done

# Workload parameters — read from environment variables (set by the agent via
# docker run -e) with hardcoded defaults for standalone use.
BATCH=${BATCH:-32}
SEQ=${SEQ:-1024}
DMODEL=${DMODEL:-4096}
DFF=${DFF:-4096}
DVOCAL=${DVOCAL:-32000}
HEAD=${HEAD:-32}
KVHEAD=${KVHEAD:-32}
NUM_STACKS=${NUM_STACKS:-32}
WEIGHT_SHARDED=${WEIGHT_SHARDED:-0}
CHAKRA_VERSION=${CHAKRA_VERSION:-v0.0.4}

#clean
rm -rf workload

SCRIPT_DIR=$(dirname "$(realpath "$0")")
PLAYGROUND_DIR="${SCRIPT_DIR:?}/../.."
PROJECT_DIR="/app/astra-sim"
EXAMPLE_DIR="${PLAYGROUND_DIR:?}/examples/llama_ftol"

# paths
STG="/app/STG"
ASTRA_SIM="${PROJECT_DIR:?}/build/astra_analytical/build/bin/AstraSim_Analytical_Congestion_Unaware"

SYSTEM="${EXAMPLE_DIR:?}/system.json"
REMOTE_MEMORY="${EXAMPLE_DIR:?}/remote_memory.json"
NETWORK="${EXAMPLE_DIR:?}/network.yml"
WORKLOAD="${EXAMPLE_DIR:?}/workload/workload"
COMM_GROUP="${EXAMPLE_DIR:?}/workload/workload.json"

mkdir -p ${EXAMPLE_DIR:?}/workload
cd ${STG:?}
python main.py --output_dir ${EXAMPLE_DIR}/workload/ \
                             --output_name workload.%d.et \
                             --tp ${TP} --dp ${DP} --pp ${PP} \
                             --dmodel ${DMODEL} --dff ${DFF} --batch ${BATCH} --seq ${SEQ} --dvocal ${DVOCAL} \
                             --head ${HEAD} --kvhead ${KVHEAD} --num_stacks ${NUM_STACKS} \
                             --weight_sharded ${WEIGHT_SHARDED} --chakra_schema_version "${CHAKRA_VERSION}" \
                             --print_gpu_vram True > ${EXAMPLE_DIR:?}/workload/workload.log 2>&1

cd -

# ── Memory check ────────────────────────────────────────────────────────────
# Parse workload.log for per-GPU total memory; fail fast if any rank exceeds GPU_MEMORY.
LOG_FILE="${EXAMPLE_DIR:?}/workload/workload.log"
OOM=0
while IFS= read -r line; do
    if [[ "$line" =~ \[GPU\ [^]]+\]\ total=([0-9]+\.[0-9]+)\ GiB ]]; then
        total="${BASH_REMATCH[1]}"
        # compare as integers scaled by 1000 to avoid bc dependency
        total_int=$(echo "$total" | awk '{printf "%d", $1 * 1000}')
        limit_int=$(echo "$GPU_MEMORY" | awk '{printf "%d", $1 * 1000}')
        if [ "$total_int" -gt "$limit_int" ]; then
            gpu_label=$(echo "$line" | grep -oP '\[GPU [^\]]+\]')
            echo "OUT OF MEMORY: $gpu_label requires ${total} GiB > ${GPU_MEMORY} GiB" >&2
            OOM=1
        fi
    fi
done < "$LOG_FILE"

if [ "$OOM" -eq 1 ]; then
    echo "out of memory" > "${EXAMPLE_DIR:?}/output.txt"
    exit 1
fi

# start
echo "[ASTRA-sim] Compiling ASTRA-sim with the Analytical Network Backend..."
echo ""

# Compile
"${PROJECT_DIR:?}"/build/astra_analytical/build.sh

echo ""
echo "[ASTRA-sim] Compilation finished."
echo "[ASTRA-sim] Running ASTRA-sim Example with Analytical Network Backend..."
echo ""

# run ASTRA-sim
export ASAN_OPTIONS=detect_container_overflow=0

"${ASTRA_SIM:?}" \
    --workload-configuration="${WORKLOAD}" \
    --system-configuration="${SYSTEM:?}" \
    --remote-memory-configuration="${REMOTE_MEMORY:?}" \
    --network-configuration="${NETWORK:?}" \
    --comm-group-configuration="${COMM_GROUP:?}" > output.txt

# Extract and report statistics for sys[0-7], filtering only [statistics]
for i in {0..7}; do
    echo ""
    echo "[ASTRA-sim] Statistics for sys[$i]:"
    grep "\[statistics\]" output.txt | grep "sys\[$i\]" | while read -r line; do
        echo "$line"
    done
done

echo ""
echo "[ASTRA-sim] Finished the execution."