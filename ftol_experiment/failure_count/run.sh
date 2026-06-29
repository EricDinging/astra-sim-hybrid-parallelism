#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(dirname "$(realpath "$0")")
PROJECT_DIR=$(realpath "${SCRIPT_DIR}/../..")
STG="${PROJECT_DIR}/extern/symbolic_tensor_graph"
BUILD_DIR=${BUILD_DIR:-/tmp/astra_ftol_failure_count_build}
BUILD_JOBS=${BUILD_JOBS:-4}
ASTRA_SIM="${BUILD_DIR}/bin/AstraSim_Analytical_Congestion_Aware"

# GPT-80B proxy: approximately 80.1B trainable parameters (tied embeddings).
TP=4
DP=8
PP=1
BATCH=32
SEQ=1024
DMODEL=8192
DFF=32768
DVOCAL=50257
HEAD=64
KVHEAD=64
NUM_STACKS=99
WEIGHT_SHARDED=1
TPSP=0
CHAKRA_VERSION=${CHAKRA_VERSION:-v0.0.4}

WORKLOAD_DIR="${SCRIPT_DIR}/workload"
RESULTS_DIR="${SCRIPT_DIR}/results"
SUMMARY="${RESULTS_DIR}/summary.csv"

mkdir -p "${WORKLOAD_DIR}" "${RESULTS_DIR}"

echo "[1/3] Building ASTRA-sim"
cmake -S "${PROJECT_DIR}/build/astra_analytical" -B "${BUILD_DIR}" \
    -DBUILDTARGET=congestion_aware -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --target AstraSim_Analytical_Congestion_Aware -j "${BUILD_JOBS}"

echo "[2/3] Generating GPT-80B workload (TP=${TP}, DP=${DP}, ${TP}x${DP}=32 GPUs)"
(cd "${STG}" && python3 main.py \
    --output_dir "${WORKLOAD_DIR}/" \
    --output_name 'workload.%d.et' \
    --tp "${TP}" --dp "${DP}" --pp "${PP}" \
    --dmodel "${DMODEL}" --dff "${DFF}" \
    --batch "${BATCH}" --seq "${SEQ}" --dvocal "${DVOCAL}" \
    --head "${HEAD}" --kvhead "${KVHEAD}" --num_stacks "${NUM_STACKS}" \
    --weight_sharded "${WEIGHT_SHARDED}" \
    --tpsp "${TPSP}" \
    --chakra_schema_version "${CHAKRA_VERSION}" \
    --print_gpu_vram true) > "${WORKLOAD_DIR}/workload.log" 2>&1

printf '%s\n' 'failure_count,wall_time_cycles,comm_time_cycles,gpu_time_cycles,overhead_cycles,overhead_percent' > "${SUMMARY}"

echo "[3/3] Sweeping failed DP-ring links from 0 through DP-1 (=7)"
baseline_wall=''
for failures in $(seq 0 $((DP - 1))); do
    run_dir="${RESULTS_DIR}/failures_${failures}"
    mkdir -p "${run_dir}"

    ASTRA_ROOT_FETCH_TOTAL_FAILURES="${failures}" \
    ASTRA_ROOT_FETCH_FAILURE_SEED=1 \
    ASTRA_ROOT_FETCH_MF=2 \
    ASTRA_ROOT_FETCH_FAIL_BEFORE_FIRST_ROUND=1 \
    ASTRA_ROOT_FETCH_DEBUG=1 \
    ASAN_OPTIONS=detect_container_overflow=0 \
    "${ASTRA_SIM}" \
        --workload-configuration="${WORKLOAD_DIR}/workload" \
        --system-configuration="${SCRIPT_DIR}/system.json" \
        --remote-memory-configuration="${SCRIPT_DIR}/remote_memory.json" \
        --network-configuration="${SCRIPT_DIR}/network.yml" \
        --comm-group-configuration="${WORKLOAD_DIR}/workload.json" \
        > "${run_dir}/output.txt" 2>&1

    wall=$(awk '/\[statistics\].*sys\[0\], Wall time:/ {print $NF; exit}' "${run_dir}/output.txt")
    comm=$(awk '/\[statistics\].*sys\[0\], Comm time:/ {print $NF; exit}' "${run_dir}/output.txt")
    gpu=$(awk '/\[statistics\].*sys\[0\], GPU time:/ {print $NF; exit}' "${run_dir}/output.txt")

    if [[ -z "${baseline_wall}" ]]; then
        baseline_wall="${wall}"
    fi
    overhead=$((wall - baseline_wall))
    overhead_pct=$(awk -v wall="${wall}" -v base="${baseline_wall}" 'BEGIN { printf "%.6f", 100.0 * (wall - base) / base }')
    printf '%s,%s,%s,%s,%s,%s\n' "${failures}" "${wall}" "${comm}" "${gpu}" "${overhead}" "${overhead_pct}" >> "${SUMMARY}"
    printf '  failures=%d wall=%s cycles overhead=%s%%\n' "${failures}" "${wall}" "${overhead_pct}"
done

echo "Results: ${SUMMARY}"
