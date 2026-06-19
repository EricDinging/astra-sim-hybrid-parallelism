#!/usr/bin/env python3
"""Pre-farm gates for one generated cell (spec section 6).

Hard gates (exit 1): dense sorted ids/arrivals; per-size mix within
max(3 sigma, 2.0) percentage points of the Pareto pmf (+ 1-node slice);
rho_isolated within +/-0.005 of target.
Soft gate (warn only): every lib shape of the cell's sizes sampled >=1x —
tail shapes legitimately miss at n=120.
"""

import argparse
import csv
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from calibrate import CAPACITY, arrival_span, job_work, read_service  # noqa: E402
from gen_mixed import scan_lib  # noqa: E402


def expected_pmf(sizes, pmf_exp, one_node_frac):
    w = {s: s**-pmf_exp for s in sizes}
    tot = sum(w.values())
    pmf = {s: (1.0 - one_node_frac) * v / tot for s, v in w.items()}
    if one_node_frac > 0:
        pmf[1] = one_node_frac
    return pmf


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("cell_dir")
    ap.add_argument("--svc", required=True)
    ap.add_argument("--lib", required=True)
    ap.add_argument("--rho", type=float, required=True)
    ap.add_argument("--sizes", required=True)
    ap.add_argument("--pmf-exp", type=float, default=1.5)
    ap.add_argument("--one-node-frac", type=float, default=0.0)
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
    pmf = expected_pmf(sizes, args.pmf_exp, args.one_node_frac)
    mix = []
    for s, p in sorted(pmf.items()):
        exp = 100.0 * p
        got = 100.0 * cnt.get(s, 0) / n
        tol = max(300.0 * (p * (1 - p) / n) ** 0.5, 2.0)
        mix.append(f"{got:.1f}")
        if abs(got - exp) > tol:
            fails.append(
                f"size {s}: {got:.1f}% vs expected {exp:.1f}% (tol {tol:.1f} pts)"
            )

    # shape coverage vs the lib (post-probe truth), warn-only
    with open(os.path.join(args.cell_dir, "job_models.csv"), newline="") as f:
        jm = list(csv.DictReader(f))
    models = sorted({r["model"] for r in jm})
    seen = Counter((r["model"], r["shape"]) for r in jm)
    missing = []
    for model in models:
        _, size_shapes = scan_lib(args.lib, model)
        for s in sorted(pmf):
            for sh in size_shapes.get(s, []):
                if not seen[(model, sh)]:
                    missing.append(f"{model}/{sh}")
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
