#!/bin/bash

# run it in docker environment
set -e

# Default values for tp, dp, and pp
TP=4
DP=2
PP=1

# Parse input arguments
while getopts "t:d:p:" opt; do
    case ${opt} in
        t ) TP=$OPTARG ;;
        d ) DP=$OPTARG ;;
        p ) PP=$OPTARG ;;
        * ) echo "Usage: $0 [-t tp] [-d dp] [-p pp]"
                exit 1 ;;
    esac
done

#clean
rm -rf workload

SCRIPT_DIR=$(dirname "$(realpath "$0")")
PLAYGROUND_DIR="${SCRIPT_DIR:?}/../.."
PROJECT_DIR="/app/astra-sim"
EXAMPLE_DIR="${PLAYGROUND_DIR:?}/examples/llama"

# paths
STG="${PROJECT_DIR:?}/symbolic_tensor_graph"
ASTRA_SIM="${PROJECT_DIR:?}/build/astra_analytical/build/bin/AstraSim_Analytical_Congestion_Unaware"

SYSTEM="${EXAMPLE_DIR:?}/system.json"
REMOTE_MEMORY="${EXAMPLE_DIR:?}/remote_memory.json"
NETWORK="${EXAMPLE_DIR:?}/network.yml"
WORKLOAD="${EXAMPLE_DIR:?}/workload/workload"
COMM_GROUP="${EXAMPLE_DIR:?}/workload/workload.json"

cd ${STG:?}
python main.py --output_dir ${EXAMPLE_DIR}/workload/ \
                             --output_name workload.%d.et \
                             --tp ${TP} --dp ${DP} --pp ${PP} \
                             --weight_sharded 0 \
                             --num_stacks 2

cd -

# Construct network.yml
# TODO change network.yml GPU count

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

# finalize
echo ""
echo "[ASTRA-sim] Finished the execution."