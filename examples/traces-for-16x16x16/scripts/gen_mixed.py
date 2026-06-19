#!/usr/bin/env python3
"""Generate one workload cell: truncated-Pareto job sizes over the even-dims
palette, with an optional fixed 1-node slice (spec section 3).

Library layout (built by `reproduce.sh lib`):
  <lib>/lat/<SHAPE>/{comm_group.json, chakra_trace.*.et}
  <lib>/bw/<SHAPE>/{...}            SHAPE like "2x4x8" (dp x tp x pp)

Per job: a 1-node coin (--one-node-frac; small cells 0.05), else a size from
pmf ~ s^-(--pmf-exp) over --sizes; a model coin (bw with prob --bw-frac);
a shape uniform over that size's shapes present in the model sub-library.
Job dirs are symlinks into the library.

Determinism contract (calibrate.py ia relies on both):
  1. the full job sequence is drawn BEFORE any arrival draw;
  2. arrival times scale linearly in --mean-ia-ns.

Writes: arrivals.csv, job_models.csv, jobs/<id> symlinks, trace_config.txt.
"""

import argparse
import csv
import os
import random


def scan_lib(lib, model):
    """SHAPE -> (abs dir, size), and size -> [shapes] for one sub-library."""
    root = os.path.join(lib, model)
    shape_dir, size_shapes = {}, {}
    if not os.path.isdir(root):
        return shape_dir, size_shapes
    for name in sorted(os.listdir(root)):
        d = os.path.join(root, name)
        if not os.path.isdir(d) or "x" not in name:
            continue
        if not os.path.isfile(os.path.join(d, "chakra_trace.0.et")):
            continue
        parts = name.split("x")
        if len(parts) != 3:
            continue
        try:
            dims = [int(p) for p in parts]
        except ValueError:
            continue
        nr = dims[0] * dims[1] * dims[2]
        shape_dir[name] = (os.path.abspath(d), nr)
        size_shapes.setdefault(nr, []).append(name)
    return shape_dir, size_shapes


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True)
    ap.add_argument("--lib", required=True)
    ap.add_argument("--n", type=int, default=120)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument(
        "--bw-frac",
        type=float,
        required=True,
        help="probability a job is bandwidth-bound (0 or 1 here)",
    )
    ap.add_argument(
        "--sizes", required=True, help="comma size grid (palette.py sizes small|large)"
    )
    ap.add_argument(
        "--pmf-exp",
        type=float,
        default=1.5,
        help="pmf ~ s^-exp (1.5 == truncated Pareto alpha=0.5)",
    )
    ap.add_argument(
        "--one-node-frac",
        type=float,
        default=0.0,
        help="fixed probability of a 1x1x1 job (small cells 0.05)",
    )
    ap.add_argument("--mean-ia-ns", type=float, required=True)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    lat_dir, lat_sizes = scan_lib(args.lib, "lat")
    bw_dir, bw_sizes = scan_lib(args.lib, "bw")
    size_grid = [int(s) for s in args.sizes.split(",")]
    pmf_w = [s**-args.pmf_exp for s in size_grid]

    need = [m for m, p in (("lat", 1 - args.bw_frac), ("bw", args.bw_frac)) if p > 0]
    for m in need:
        have = bw_sizes if m == "bw" else lat_sizes
        missing = [s for s in size_grid if s not in have]
        if args.one_node_frac > 0 and 1 not in have:
            missing.insert(0, 1)
        if missing:
            raise SystemExit(f"sizes missing from {m} lib: {missing}")

    # job sequence first (determinism contract, point 1)
    jobs = []
    for _ in range(args.n):
        one = rng.random() < args.one_node_frac  # always consumes one draw
        size = 1 if one else rng.choices(size_grid, weights=pmf_w, k=1)[0]
        model = "bw" if rng.random() < args.bw_frac else "lat"
        shapes = (bw_sizes if model == "bw" else lat_sizes)[size]
        shape = rng.choice(shapes)
        target = (bw_dir if model == "bw" else lat_dir)[shape][0]
        jobs.append((model, size, shape, target))

    # Poisson arrivals (determinism contract, point 2: linear in mean-ia)
    times, t = [], 0.0
    for _ in range(args.n):
        times.append(t)
        t += rng.expovariate(1.0 / args.mean_ia_ns)
    times = [int(round(x)) for x in times]

    order = sorted(range(args.n), key=lambda i: times[i])
    jobs_dir = os.path.join(args.out, "jobs")
    os.makedirs(jobs_dir, exist_ok=True)
    for nm in os.listdir(jobs_dir):
        p = os.path.join(jobs_dir, nm)
        if os.path.islink(p):
            os.unlink(p)

    with (
        open(os.path.join(args.out, "arrivals.csv"), "w", newline="") as f,
        open(os.path.join(args.out, "job_models.csv"), "w", newline="") as mf,
    ):
        w, mw = csv.writer(f), csv.writer(mf)
        w.writerow(["job_id", "arrival_time_ns", "num_ranks", "shape"])
        mw.writerow(["job_id", "model", "num_ranks", "shape"])
        for jid, oi in enumerate(order):
            model, size, shape, target = jobs[oi]
            w.writerow([jid, times[oi], size, shape])
            mw.writerow([jid, model, size, shape])
            os.symlink(target, os.path.join(jobs_dir, str(jid)))

    nbw = sum(1 for m, *_ in jobs if m == "bw")
    none = sum(1 for _, s, *_ in jobs if s == 1)
    with open(os.path.join(args.out, "trace_config.txt"), "w") as f:
        for k, v in sorted(vars(args).items()):
            f.write(f"{k}={v}\n")
        f.write(f"actual_bw_jobs={nbw}\nactual_lat_jobs={args.n - nbw}\n")
        f.write(f"actual_one_node_jobs={none}\n")
    print(
        f"wrote {args.out}: {args.n} jobs ({nbw} bw, {none} one-node), "
        f"{len(set(s for _, _, s, _ in jobs))} distinct shapes"
    )


if __name__ == "__main__":
    main()
