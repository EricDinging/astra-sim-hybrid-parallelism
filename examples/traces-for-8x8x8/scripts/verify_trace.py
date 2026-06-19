#!/usr/bin/env python3
"""Pre-farm gates for one generated cell (spec section 10).

Hard gates (exit 1): dense sorted ids/arrivals, size mix within tolerance of
the Pareto pmf, rho_isolated within +/-0.005 of target.
Soft gate (warn only): every factorization appears >=1x -- with 400 draws a
rare shape can legitimately miss (e.g. each size-16 shape has ~0.85% miss
probability), so absence is reported but not fatal.
"""

import argparse
import csv
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from calibrate import CAPACITY, arrival_span, job_work, read_service  # noqa: E402
from palette import shapes_for  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cell_dir")
    ap.add_argument("--svc", required=True)
    ap.add_argument("--rho", type=float, required=True)
    ap.add_argument("--sizes", required=True, help="e.g. 4,8,16")
    # 3 sigma of the widest pmf bin (p=4/7, n=400): catches structurally wrong
    # weights (tens of points off) while passing fixed-seed sampling variance
    # (seed 1 puts size-64 +2.4 sigma in the large cells).
    ap.add_argument("--mix-tol", type=float, default=7.5, help="percentage points")
    args = ap.parse_args()
    sizes = [int(s) for s in args.sizes.split(",")]
    name = os.path.basename(args.cell_dir.rstrip("/"))
    fails, warns = [], []

    with open(os.path.join(args.cell_dir, "arrivals.csv"), newline="") as f:
        rows = list(csv.DictReader(f))
    ids = [int(r["job_id"]) for r in rows]
    times = [int(r["arrival_time_ns"]) for r in rows]
    if ids != list(range(len(ids))):
        fails.append("job ids not dense 0..n-1")
    if times != sorted(times):
        fails.append("arrivals not sorted")

    n = len(rows)
    cnt = Counter(int(r["num_ranks"]) for r in rows)
    tot = sum(1.0 / s for s in sizes)
    mix = []
    for s in sizes:
        exp = 100.0 * (1.0 / s) / tot
        got = 100.0 * cnt.get(s, 0) / n
        mix.append(f"{got:.1f}")
        if abs(got - exp) > args.mix_tol:
            fails.append(
                f"size {s}: {got:.1f}% vs expected {exp:.1f}% (tol {args.mix_tol} pts)"
            )

    seen = Counter(r["shape"] for r in rows)
    missing = [sh for s in sizes for sh in shapes_for(s) if not seen[sh]]
    if missing:
        warns.append(
            "shapes never sampled (statistically possible): " + " ".join(missing)
        )

    svc = read_service(args.svc)
    rho = job_work(args.cell_dir, svc) / (CAPACITY * arrival_span(args.cell_dir))
    if abs(rho - args.rho) > 0.005:
        fails.append(f"rho_isolated={rho:.4f} vs target {args.rho} (tol 0.005)")

    for w in warns:
        print(f"{name}: WARN {w}")
    if fails:
        print(f"{name}: FAIL")
        for x in fails:
            print(f"  - {x}")
        sys.exit(1)
    print(
        f"{name}: OK rho_isolated={rho:.4f} n={n} mix={'/'.join(mix)} "
        f"shapes={len(seen)}"
    )


if __name__ == "__main__":
    main()
