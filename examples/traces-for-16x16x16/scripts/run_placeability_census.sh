#!/bin/bash
# Placeability census driver: for every legitimate job shape, ask each
# placement policy whether it places on a (degraded-)idle 16x16x16 torus,
# then emit one CSV per failure rate via make_placeability_csvs.py.
#
# Legitimate shapes (a<=b<=c, product<=4096): 1x1xc with c even,
# 1xbxc with b,c even, axbxc with a,b,c even — 8,204 shapes.
#
# Policies: ff (FirstFit), b16 (RFold whole-torus = pure folding),
# b8/b4/b2/b1 (RFold with that block size). ff runs via try_place;
# RFold policies run via the probe's oracle mode (see placeability_probe.cc
# for the equivalence argument and validation).
#
# Usage: run_placeability_census.sh [workdir]
#   FAILURE_RATES  override the swept rates   (default "0 0.001 0.002 0.005 0.01")
#   WORKERS        parallel probe processes   (default: nproc-4)
#   BUILD_DIR      build tree with libAstraSim.a (default: build_tests)
#   OUTDIR         where the CSVs land (default: traces-for-16x16x16/)
#
# The sweep is resumable: rerunning skips shapes already answered in the
# workdir's raw files (self-healing mop loop).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WORK="${1:-/tmp/placeability_census}"
BUILD_DIR="${BUILD_DIR:-$REPO/build/astra_analytical/build_tests}"
WORKERS="${WORKERS:-$(($(nproc) - 4))}"
FAILURE_RATES="${FAILURE_RATES:-0 0.001 0.002 0.005 0.01}"
POLICIES="ff b16 b8 b4 b2 b1"
PROBE="$WORK/placeability_probe"
SHAPES="$WORK/legit_shapes.txt"

mkdir -p "$WORK/raw"

# --- build the probe against the prebuilt test libraries -------------------
if [ ! -x "$PROBE" ] || [ "$SCRIPT_DIR/placeability_probe.cc" -nt "$PROBE" ]; then
    echo "building probe..."
    EXT="$REPO/extern/network_backend/analytical"
    c++ -DSPDLOG_COMPILED_LIB -DYAML_CPP_STATIC_DEFINE \
        -I"$BUILD_DIR" -I"$REPO" \
        -I"$EXT/extern/yaml-cpp/include" \
        -I"$REPO/astra-sim/network_frontend/analytical/include" \
        -I"$EXT/include" -I"$EXT/include/astra-network-analytical" \
        -I"$REPO/extern/graph_frontend/chakra" \
        -I"$REPO/extern/graph_frontend/chakra/schema/protobuf" \
        -I"$REPO/extern/graph_frontend/chakra/src/third_party/utils" \
        -I"$REPO/extern/helper/fmt/include" \
        -I"$REPO/extern/helper/spdlog/include" \
        -isystem "$BUILD_DIR/AstraSim/topomatch-install/include" \
        -isystem /usr/include/scotch \
        -Wno-error -O2 -std=gnu++17 \
        "$SCRIPT_DIR/placeability_probe.cc" -o "$PROBE" \
        "$BUILD_DIR/lib/libAstraSim.a" \
        "$BUILD_DIR/lib/libAnalytical_Reconfigurable.a" \
        "$BUILD_DIR/AstraSim/extern/helper/fmt/libfmt.a" \
        "$BUILD_DIR/AstraSim/extern/helper/spdlog/libspdlog.a" \
        /usr/lib/x86_64-linux-gnu/libprotobuf.so \
        "$BUILD_DIR/AstraSim/topomatch-install/lib/libtopomatch.a" \
        /usr/lib/x86_64-linux-gnu/libscotch.so \
        /usr/lib/x86_64-linux-gnu/libscotcherr.so \
        /usr/lib/x86_64-linux-gnu/libhwloc.so -lz -lpthread \
        "$BUILD_DIR/Analytical/yaml-cpp/libyaml-cpp.a"
fi

# --- enumerate the legitimate shape universe --------------------------------
# (kept if already present, so a pre-seeded subset can be swept for testing)
[ -s "$SHAPES" ] || python3 - "$SHAPES" <<'PYEOF'
import sys
N = 4096
with open(sys.argv[1], "w") as f:
    for c in range(2, N + 1, 2):
        f.write(f"1 1 {c}\n")
    b = 2
    while b * b <= N:
        for c in range(b, N // b + 1, 2):
            f.write(f"1 {b} {c}\n")
        b += 2
    a = 2
    while a * a * a <= N:
        b = a
        while a * b * b <= N:
            for c in range(b, N // (a * b) + 1, 2):
                f.write(f"{a} {b} {c}\n")
            b += 2
        a += 2
PYEOF
echo "shape universe: $(wc -l < "$SHAPES") shapes"

# --- sweep: per rate, per policy, run only not-yet-answered shapes ----------
rate_tag() { echo "$1" | sed 's/^0\.//; s/^0$/idle/'; }

for rate in $FAILURE_RATES; do
    tag="$(rate_tag "$rate")"
    for pol in $POLICIES; do
        miss="$WORK/missing_${pol}_${tag}.txt"
        python3 - "$WORK/raw" "$pol" "$tag" "$SHAPES" "$miss" <<'PYEOF'
import glob
import sys

raw, pol, tag, shapes_fn, miss_fn = sys.argv[1:6]
done = set()
for fn in glob.glob(f"{raw}/{pol}_{tag}.*.txt"):
    for line in open(fn):
        p = line.split()
        if len(p) >= 4 and p[3] in ("PLACED", "DROP", "DEFER", "NOPE"):
            done.add((p[0], p[1], p[2]))
missing = [s for s in (tuple(l.split()) for l in open(shapes_fn))
           if s not in done]
with open(miss_fn, "w") as f:
    for s in missing:
        f.write(" ".join(s) + "\n")
print(f"{pol} rate={tag}: {len(missing)} to run", flush=True)
PYEOF
        if [ -s "$miss" ]; then
            mode=oraclefile
            [ "$pol" = ff ] && mode=tryfile
            seq 0 $((WORKERS - 1)) | xargs -P "$WORKERS" -I SH sh -c \
                "'$PROBE' $pol $mode '$miss' SH $WORKERS $rate \
                 > '$WORK/raw/${pol}_${tag}.mop$(date +%s)_SH.txt' 2>/dev/null"
        fi
    done
    echo "rate $rate done: $(date +%T)"
done

# --- aggregate to CSVs -------------------------------------------------------
python3 "$SCRIPT_DIR/make_placeability_csvs.py" \
    --raw "$WORK/raw" --shapes "$SHAPES" \
    --rates "$FAILURE_RATES" --outdir "${OUTDIR:-$SCRIPT_DIR/..}"
