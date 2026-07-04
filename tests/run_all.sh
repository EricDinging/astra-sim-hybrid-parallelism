#!/bin/bash
set -e

# Path
SCRIPT_DIR=$(dirname "$(realpath $0)")

echo "[$0] Running all regression tests..."

echo "[$0] Running rt_template..."
${SCRIPT_DIR}/rt_template/run.sh || (echo "Failed." ; exit 1)

echo "[$0] Running rt_sync_node_strand..."
${SCRIPT_DIR}/rt_sync_node_strand/run.sh || (echo "Failed." ; exit 1)

echo "[$0] Running rt_ordinal_divergence..."
${SCRIPT_DIR}/rt_ordinal_divergence/run.sh || (echo "Failed." ; exit 1)

echo "[$0] Finished all regression tests."