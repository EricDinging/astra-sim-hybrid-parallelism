#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(dirname "$(realpath "$0")")
PROJECT_DIR=$(realpath "${SCRIPT_DIR}/../..")
STG="${PROJECT_DIR}/extern/symbolic_tensor_graph"
BUILD_DIR=${BUILD_DIR:-/tmp/astra_ftol_gpu_count_build}
BUILD_JOBS=${BUILD_JOBS:-4}
ASTRA_SIM="${BUILD_DIR}/bin/AstraSim_Analytical_Congestion_Aware"

# GPT-80B proxy: approximately 80.1B trainable parameters (tied embeddings).
TP=${TP:-4}
PP=${PP:-1}
DP_VALUES=${DP_VALUES:-"4 8 16"}
FAILURES=${FAILURES:-2}
BATCH=${BATCH:-32}
SEQ=${SEQ:-1024}
DMODEL=${DMODEL:-8192}
DFF=${DFF:-32768}
DVOCAL=${DVOCAL:-50257}
HEAD=${HEAD:-64}
KVHEAD=${KVHEAD:-64}
NUM_STACKS=${NUM_STACKS:-99}
WEIGHT_SHARDED=${WEIGHT_SHARDED:-1}
TPSP=${TPSP:-0}
CHAKRA_VERSION=${CHAKRA_VERSION:-v0.0.4}
RAIL_BW=${RAIL_BW:-50}
RAIL_LATENCY=${RAIL_LATENCY:-500.0}

WORKLOAD_ROOT="${SCRIPT_DIR}/workload"
RESULTS_DIR="${SCRIPT_DIR}/results"
SUMMARY="${RESULTS_DIR}/summary.csv"

mkdir -p "${WORKLOAD_ROOT}" "${RESULTS_DIR}"

printf '[1/3] Building ASTRA-sim\n'
cmake -S "${PROJECT_DIR}/build/astra_analytical" -B "${BUILD_DIR}" \
    -DBUILDTARGET=congestion_aware -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --target AstraSim_Analytical_Congestion_Aware -j "${BUILD_JOBS}"

printf '%s\n' 'dp,gpu_count,failure_count,baseline_wall_time_cycles,wall_time_cycles,comm_time_cycles,gpu_time_cycles,overhead_cycles,overhead_percent' > "${SUMMARY}"

for DP in ${DP_VALUES}; do
    GPU_COUNT=$((TP * DP * PP))
    if (( FAILURES > DP - 1 )); then
        printf 'FAILURES=%d is larger than DP-1=%d for DP=%d\n' "${FAILURES}" "$((DP - 1))" "${DP}" >&2
        exit 1
    fi

    workload_dir="${WORKLOAD_ROOT}/dp${DP}"
    mkdir -p "${workload_dir}"

    printf '[2/3] Generating workload for DP=%d (%d GPUs)\n' "${DP}" "${GPU_COUNT}"
    (cd "${STG}" && python3 main.py \
        --output_dir "${workload_dir}/" \
        --output_name 'workload.%d.et' \
        --tp "${TP}" --dp "${DP}" --pp "${PP}" \
        --dmodel "${DMODEL}" --dff "${DFF}" \
        --batch "${BATCH}" --seq "${SEQ}" --dvocal "${DVOCAL}" \
        --head "${HEAD}" --kvhead "${KVHEAD}" --num_stacks "${NUM_STACKS}" \
        --weight_sharded "${WEIGHT_SHARDED}" \
        --tpsp "${TPSP}" \
        --chakra_schema_version "${CHAKRA_VERSION}" \
        --print_gpu_vram true) > "${workload_dir}/workload.log" 2>&1

    run_dir="${RESULTS_DIR}/dp${DP}"
    mkdir -p "${run_dir}"
    cat > "${run_dir}/network.yml" <<NETEOF
topology: [ RailNvlink ]
npus_count: [ ${GPU_COUNT} ]
bandwidth: [ ${RAIL_BW} ]
latency: [ ${RAIL_LATENCY} ]
NETEOF

    printf '[3/3] Running DP=%d baseline and %d-failure case\n' "${DP}" "${FAILURES}"
    for failure_count in 0 "${FAILURES}"; do
        case_dir="${run_dir}/failures_${failure_count}"
        mkdir -p "${case_dir}"

        ASTRA_ROOT_FETCH_TOTAL_FAILURES="${failure_count}" \
        ASTRA_ROOT_FETCH_FAILURE_SEED=1 \
        ASTRA_ROOT_FETCH_MF=2 \
        ASTRA_ROOT_FETCH_FAIL_BEFORE_FIRST_ROUND=1 \
        ASTRA_ROOT_FETCH_DEBUG=1 \
        ASAN_OPTIONS=detect_container_overflow=0 \
        "${ASTRA_SIM}" \
            --workload-configuration="${workload_dir}/workload" \
            --system-configuration="${SCRIPT_DIR}/system.json" \
            --remote-memory-configuration="${SCRIPT_DIR}/remote_memory.json" \
            --network-configuration="${run_dir}/network.yml" \
            --comm-group-configuration="${workload_dir}/workload.json" \
            > "${case_dir}/output.txt" 2>&1
    done

    baseline_output="${run_dir}/failures_0/output.txt"
    failed_output="${run_dir}/failures_${FAILURES}/output.txt"
    baseline_wall=$(awk '/\[statistics\].*sys\[0\], Wall time:/ {print $NF; exit}' "${baseline_output}")
    wall=$(awk '/\[statistics\].*sys\[0\], Wall time:/ {print $NF; exit}' "${failed_output}")
    comm=$(awk '/\[statistics\].*sys\[0\], Comm time:/ {print $NF; exit}' "${failed_output}")
    gpu=$(awk '/\[statistics\].*sys\[0\], GPU time:/ {print $NF; exit}' "${failed_output}")
    overhead=$((wall - baseline_wall))
    overhead_pct=$(awk -v wall="${wall}" -v base="${baseline_wall}" 'BEGIN { printf "%.6f", 100.0 * (wall - base) / base }')
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${DP}" "${GPU_COUNT}" "${FAILURES}" "${baseline_wall}" "${wall}" \
        "${comm}" "${gpu}" "${overhead}" "${overhead_pct}" >> "${SUMMARY}"
    printf '  DP=%d GPUs=%d failures=%d wall=%s cycles overhead=%s%%\n' \
        "${DP}" "${GPU_COUNT}" "${FAILURES}" "${wall}" "${overhead_pct}"
done

printf 'Results: %s\n' "${SUMMARY}"
