#!/bin/bash
set -e

SCRIPT_DIR=$(dirname "$(realpath "$0")")
PROJECT_DIR="${SCRIPT_DIR:?}/../.."
EXAMPLE_DIR="${PROJECT_DIR:?}/examples/stg-ep2-tp2-pp2"

# paths
STG="${PROJECT_DIR:?}/extern/symbolic_tensor_graph"
ASTRA_SIM="${PROJECT_DIR:?}/build/astra_analytical/build/bin/AstraSim_Analytical_Congestion_Unaware"
WORKLOAD="${EXAMPLE_DIR:?}/workload"
SYSTEM="${EXAMPLE_DIR:?}/system.json"
NETWORK="${EXAMPLE_DIR:?}/network.yml"
REMOTE_MEMORY="${EXAMPLE_DIR:?}/remote_memory.json"
COMM_GROUP="${EXAMPLE_DIR:?}/workload.json"

cd ${STG:?}
python main.py --output_dir ${EXAMPLE_DIR}/workload/ \
               --output_name workload.%d.et \
               --ep 2 --dp 2 --pp 2 \
               --weight_sharded 0 \
               --num_stacks 2

cd -
