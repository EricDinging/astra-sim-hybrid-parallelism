#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(dirname "$(realpath "$0")")
PROJECT_DIR=$(realpath "${SCRIPT_DIR}/../..")
STG="${PROJECT_DIR}/extern/symbolic_tensor_graph"
BUILD_DIR=${BUILD_DIR:-/tmp/astra_ftol_failure_count_multiseed_build}
BUILD_JOBS=${BUILD_JOBS:-4}
ASTRA_SIM="${BUILD_DIR}/bin/AstraSim_Analytical_Congestion_Aware"

TP=${TP:-4}
DP=${DP:-8}
PP=${PP:-1}
SEEDS=${SEEDS:-"1 2 3 4 5"}
FAILURE_VALUES=${FAILURE_VALUES:-"0 1 2 3 4 5 6 7"}
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

WORKLOAD_DIR="${SCRIPT_DIR}/workload"
RESULTS_DIR="${SCRIPT_DIR}/results_multiseed"
SUMMARY="${RESULTS_DIR}/summary.csv"

mkdir -p "${WORKLOAD_DIR}" "${RESULTS_DIR}"

printf '[1/3] Building ASTRA-sim\n'
cmake -S "${PROJECT_DIR}/build/astra_analytical" -B "${BUILD_DIR}" \
    -DBUILDTARGET=congestion_aware -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --target AstraSim_Analytical_Congestion_Aware -j "${BUILD_JOBS}"

printf '[2/3] Generating GPT-80B workload (TP=%d, DP=%d, %d GPUs)\n' "${TP}" "${DP}" "$((TP * DP * PP))"
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

printf '%s\n' 'failure_count,seed,wall_time_cycles,comm_time_cycles,gpu_time_cycles,overlap_cycles,protocol_transfers' > "${SUMMARY}"

printf '[3/3] Sampling failures with seeds: %s\n' "${SEEDS}"
for failures in ${FAILURE_VALUES}; do
    for seed in ${SEEDS}; do
        run_dir="${RESULTS_DIR}/failures_${failures}/seed_${seed}"
        mkdir -p "${run_dir}"

        ASTRA_ROOT_FETCH_TOTAL_FAILURES="${failures}" \
        ASTRA_ROOT_FETCH_FAILURE_SEED="${seed}" \
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
        overlap=$(awk '/\[statistics\].*sys\[0\], Total compute-communication overlap:/ {print $NF; exit}' "${run_dir}/output.txt")
        transfers=$(awk '/\[root-fetch-config\]/ {for (i=1; i<=NF; ++i) if ($i ~ /^transfers=/) {split($i,a,"="); print a[2]; exit}}' "${run_dir}/output.txt")
        printf '%s,%s,%s,%s,%s,%s,%s\n' "${failures}" "${seed}" "${wall}" "${comm}" "${gpu}" "${overlap}" "${transfers}" >> "${SUMMARY}"
        printf '  failures=%s seed=%s wall=%s cycles\n' "${failures}" "${seed}" "${wall}"
    done
done

printf 'Results: %s\n' "${SUMMARY}"
