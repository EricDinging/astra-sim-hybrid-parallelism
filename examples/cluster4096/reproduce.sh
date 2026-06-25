#!/usr/bin/env bash
#
# Top-level reproduce driver for the cluster4096 (16x16x16 / 4096-node) torus
# experiments. Dispatches to helpers in scripts/. Each phase is self-contained.
#
# Usage: ./reproduce.sh <phase>
# Phases:
#   traces    Build the full Chakra trace library (all legal bw shapes) via stage.
#   # configs | calibrate | gen | run | analyze  -- added by later helpers
#
# Env:
#   STAGE_IMAGE  Docker image bundling stage (default: astra:latest)
#   DOCKER     Docker command            (default: sudo docker)
#   JOBS       In-container parallelism  (default: nproc-2, chosen by the helper)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAGE_IMAGE="${STAGE_IMAGE:-astra:latest}"
DOCKER="${DOCKER:-sudo docker}"
JOBS="${JOBS:-}"

usage() {
  sed -n '3,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

phase="${1:-}"
case "$phase" in
  traces)
    python3 "$HERE/scripts/gen_traces.py" \
      --out "$HERE/tracelib" \
      --image "$STAGE_IMAGE" \
      --docker "$DOCKER" \
      ${JOBS:+--jobs "$JOBS"}
    ;;
  "" | -h | --help | help)
    usage
    [ -z "$phase" ] && exit 1 || exit 0
    ;;
  *)
    echo "unknown phase: $phase" >&2
    usage
    exit 1
    ;;
esac
