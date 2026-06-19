#!/bin/bash
set -e 

SCRIPT_DIR=$(dirname "$(realpath $0)")
PROJ_DIR=${SCRIPT_DIR}/../..

### ============= Run Regression Tests ==================
${PROJ_DIR}/tests/run_all.sh

### ============= Run Unit Tests ==================
${PROJ_DIR}/tests/run_unit_tests.sh

### ======================================================