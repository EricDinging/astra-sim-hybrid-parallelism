#!/bin/bash
set -e

## ******************************************************************************
## This source code is licensed under the MIT license found in the
## LICENSE file in the root directory of this source tree.
##
## Copyright (c) 2024 Georgia Institute of Technology
## ******************************************************************************

# find the absolute path to this script
SCRIPT_DIR=$(dirname "$(realpath "$0")")
PROJECT_DIR="${SCRIPT_DIR:?}/../.."
EXAMPLE_DIR="${PROJECT_DIR:?}/examples/stg-dp2-tp2"

# paths
ASTRA_SIM="${PROJECT_DIR:?}/build/astra_analytical/build/bin/AstraSim_Analytical_Reconfigurable"
WORKLOAD="${EXAMPLE_DIR:?}/workload"
SYSTEM="${EXAMPLE_DIR:?}/system.json"
NETWORK_CONFIGS="${EXAMPLE_DIR:?}/network_config"
REMOTE_MEMORY="${EXAMPLE_DIR:?}/remote_memory.json"
COMM_GROUP="${EXAMPLE_DIR:?}/workload.json"
CIRCUIT_SCHEDULES="${EXAMPLE_DIR:?}/schedules.txt"

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
export ASAN_OPTIONS=detect_container_overflow=0:detect_leaks=0

for NETWORK in "${NETWORK_CONFIGS}"/*; do
    NETWORK_NAME=$(basename "${NETWORK}")
    OUTPUT_FILE="${EXAMPLE_DIR}/output/debug_${NETWORK_NAME}"
    "${ASTRA_SIM:?}" \
        --workload-configuration="${WORKLOAD}" \
        --system-configuration="${SYSTEM:?}" \
        --remote-memory-configuration="${REMOTE_MEMORY:?}" \
        --network-configuration="${NETWORK:?}" \
        --comm-group-configuration="${COMM_GROUP:?}" \
        --circuit-schedules="${CIRCUIT_SCHEDULES:?}" > "${OUTPUT_FILE}" 2>&1
done

# finalize
echo ""
echo "[ASTRA-sim] Finished the execution."
