#!/bin/bash

# NVIDIA Vera Rubin NVL72 rack
# LLaMA-8B, batch=128, seq=8192
# Sweeps TP scaling (DP=1) and DP scaling (TP=1) over 1,2,4,8,16,32,64 GPUs


SCRIPT_DIR=$(dirname "$(realpath "$0")")
PLAYGROUND_DIR="${SCRIPT_DIR}/../.."
PROJECT_DIR="/app/astra-sim"
EXAMPLE_DIR="${SCRIPT_DIR}"

STG="/app/STG"
ASTRA_SIM="${PROJECT_DIR}/build/astra_analytical/build/bin/AstraSim_Analytical_Congestion_Unaware"

SYSTEM="${EXAMPLE_DIR}/system.json"
REMOTE_MEMORY="${EXAMPLE_DIR}/remote_memory.json"

# LLaMA-8B parameters
BATCH=64
SEQ=2048
DMODEL=4096
DFF=14336
DVOCAL=128256
HEAD=32
KVHEAD=8
NUM_STACKS=32
WEIGHT_SHARDED=0
CHAKRA_VERSION=${CHAKRA_VERSION:-v0.0.4}

PP=1
SCALE_VALS=(1 2 4 8 16 32 64)

# Compile once upfront
echo "[ASTRA-sim] Compiling..."
"${PROJECT_DIR}"/build/astra_analytical/build.sh
echo "[ASTRA-sim] Compilation done."

run_experiment() {
    local TP=$1
    local DP=$2
    local LABEL=$3
    local TOTAL_GPUS=$(( TP * DP * PP ))

    local OUT_DIR="${EXAMPLE_DIR}/results/${LABEL}"
    mkdir -p "${OUT_DIR}"

    echo ""
    echo "=== Running ${LABEL}: TP=${TP} DP=${DP} PP=${PP} total_gpus=${TOTAL_GPUS} ==="

    # Generate workload
    local WL_DIR="${OUT_DIR}/workload"
    mkdir -p "${WL_DIR}"

    cd "${STG}"
    python main.py \
        --output_dir "${WL_DIR}/" \
        --output_name workload.%d.et \
        --tp ${TP} --dp ${DP} --pp ${PP} \
        --dmodel ${DMODEL} --dff ${DFF} --batch ${BATCH} --seq ${SEQ} \
        --dvocal ${DVOCAL} --head ${HEAD} --kvhead ${KVHEAD} \
        --num_stacks ${NUM_STACKS} --weight_sharded ${WEIGHT_SHARDED} \
        --chakra_schema_version "${CHAKRA_VERSION}" \
        --print_gpu_vram True > "${WL_DIR}/workload.log" 2>&1
    cd -

    # Write per-run network config with correct npus_count
    # Analytical backend requires npus_count > 1; single-GPU runs use 2 (no comms issued anyway)
    local NET_NPUS=$(( TOTAL_GPUS > 1 ? TOTAL_GPUS : 2 ))
    if [ "${TOTAL_GPUS}" -eq 1 ]; then
        cp "${WL_DIR}/workload.0.et" "${WL_DIR}/workload.1.et"
    fi
    local NET="${OUT_DIR}/network.yml"
    cat > "${NET}" <<EOF
topology: [ Switch ]
npus_count: [ ${NET_NPUS} ]
bandwidth: [ 1800 ]
latency: [ 100.0 ]
EOF

    # Run ASTRA-sim
    export ASAN_OPTIONS=detect_container_overflow=0
    "${ASTRA_SIM}" \
        --workload-configuration="${WL_DIR}/workload" \
        --system-configuration="${SYSTEM}" \
        --remote-memory-configuration="${REMOTE_MEMORY}" \
        --network-configuration="${NET}" \
        --comm-group-configuration="${WL_DIR}/workload.json" \
        > "${OUT_DIR}/output.txt" 2>&1

    # Extract wall time for sys[0]
    local wall_time
    wall_time=$(grep "\[statistics\]" "${OUT_DIR}/output.txt" 2>/dev/null | grep "sys\[0\]" | grep "Wall time" | awk '{print $NF}' || echo "ERROR")
    echo "  Wall time (sys[0]): ${wall_time} cycles"
    echo "tp=${TP} dp=${DP} wall_time=${wall_time}" >> "${EXAMPLE_DIR}/results/summary.txt"
}

# Clear summary
mkdir -p "${EXAMPLE_DIR}/results"
echo "# label tp dp wall_time_cycles" > "${EXAMPLE_DIR}/results/summary.txt"

echo ""
echo "========== TP SCALING (DP=1) =========="
for N in "${SCALE_VALS[@]}"; do
    run_experiment "${N}" 1 "tp_scaling/tp${N}"
done

echo ""
echo "========== DP SCALING (TP=1) =========="
for N in "${SCALE_VALS[@]}"; do
    run_experiment 1 "${N}" "dp_scaling/dp${N}"
done

echo ""
echo "========== DONE =========="
echo "Results saved under: ${EXAMPLE_DIR}/results/"
echo "Summary: ${EXAMPLE_DIR}/results/summary.txt"
