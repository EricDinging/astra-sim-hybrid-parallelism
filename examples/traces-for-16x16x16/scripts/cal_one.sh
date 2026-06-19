#!/bin/bash
# cal_one.sh <model> <shape> <size> — one isolated calibration sim on this
# host. Prints "model,shape,size,svc_ns" as the LAST stdout line. Runs from
# the shipped REMOTE_DIR tree (this script lives in its scripts/).
set -euo pipefail
MODEL="$1"; SHAPE="$2"; SIZE="$3"
RD=$(realpath "$(dirname "$(realpath "$0")")/..")
d="$RD/calruns/${MODEL}_${SHAPE}"
rm -rf "$d"; mkdir -p "$d/jobs"
printf 'job_id,arrival_time_ns,num_ranks,shape\n0,0,%s,%s\n' \
  "$SIZE" "$SHAPE" > "$d/arrivals.csv"
ln -s "$RD/mixlib/$MODEL/$SHAPE" "$d/jobs/0"
OUT="$d/output/firstfit" \
  ASTRA_SIM="$RD/bin/AstraSim_Analytical_Reconfigurable" CFG="$RD/configs" \
  timeout 21600 bash "$RD/scripts/run_policy.sh" "$d" fifo firstfit
python3 - "$d/output/firstfit/jobs.csv" "$MODEL" "$SHAPE" "$SIZE" <<'EOF'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
assert len(rows) == 1 and rows[0]["status"] == "COMPLETED", rows
assert int(rows[0]["queue_wait_ns"]) == 0, "nonzero queue wait in isolation"
print(f"{sys.argv[2]},{sys.argv[3]},{sys.argv[4]},{int(rows[0]['jct_ns'])}")
EOF
rm -rf "$d"
