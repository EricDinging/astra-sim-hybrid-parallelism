#!/bin/bash
set -e

SCRIPT_DIR=$(dirname "$(realpath "$0")")
REPO_ROOT="${SCRIPT_DIR}/.."
BUILD_DIR="${REPO_ROOT}/build/astra_analytical/build_tests"

echo "[$0] Configuring unit-test build in ${BUILD_DIR}..."
cmake -S "${REPO_ROOT}/build/astra_analytical" -B "${BUILD_DIR}" -DBUILD_UNIT_TESTS=ON

echo "[$0] Building unit tests..."
cmake --build "${BUILD_DIR}" --parallel

echo "[$0] Running unit tests..."
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "[$0] Ok."
