#!/usr/bin/env python3
"""Generate a HETEROGENEOUS 400-job trace mixing bandwidth-bound and
latency-bound jobs over a diverse shape/size palette.

Library layout (built by STG):
  <lib>/lat/<SHAPE>/{comm_group.json, chakra_trace.*.et}   (small model, latency-bound)
  <lib>/bw/<SHAPE>/{...}                                    (big-activation model, bandwidth-bound)
where SHAPE is like "2x4x8" and num_ranks = product of dims.

Each job is assigned a model (bw with probability --bw-frac, else lat), a size
(from --size-weights, skewed ok), and a shape of that size available in the
chosen model's sub-library (sampled uniformly for diversity). The job dir is a
symlink into the right (model, shape) library entry.

Writes:
  <out>/arrivals.csv        job_id,arrival_time_ns,num_ranks,shape
  <out>/jobs/<id> -> symlink
  <out>/job_models.csv      job_id,model,num_ranks,shape   (for tail analysis)
  <out>/trace_config.txt
"""
import argparse
import csv
import os
import random


def scan_lib(lib, model):
    """SHAPE -> abs dir, and size -> [shapes] for one model sub-library."""
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
        dims = [int(x) for x in name.split("x")]
        nr = dims[0] * dims[1] * dims[2]
        shape_dir[name] = (os.path.abspath(d), nr)
        size_shapes.setdefault(nr, []).append(name)
    return shape_dir, size_shapes


def parse_weights(spec):
    w = {}
    for tok in spec.split(","):
        tok = tok.strip()
        if tok:
            k, v = tok.split(":")
            w[int(k)] = float(v)
    return w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--lib", required=True)
    ap.add_argument("--n", type=int, default=400)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--bw-frac", type=float, default=0.2,
                    help="fraction of bandwidth-bound jobs")
    ap.add_argument("--bw-max-size", type=int, default=10**9,
                    help="bandwidth-bound jobs only at sizes <= this (bounds "
                         "their JCT so they don't own the p99 tail / HOL-block)")
    ap.add_argument("--size-weights", default="8:3,16:3,32:2,64:2,128:1,256:0.4",
                    help="skewed size distribution (many small, few large)")
    ap.add_argument("--arrival-dist", choices=["poisson", "bursty"], default="poisson")
    ap.add_argument("--mean-ia-ns", type=float, default=2e6)
    ap.add_argument("--burst-size", type=int, default=16)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    lat_dir, lat_sizes = scan_lib(args.lib, "lat")
    bw_dir, bw_sizes = scan_lib(args.lib, "bw")
    if not lat_dir:
        raise SystemExit(f"no lat library under {args.lib}/lat")
    weights = parse_weights(args.size_weights)
    sizes = [s for s in weights if s in lat_sizes]
    probs = [weights[s] for s in sizes]

    # assign each job a (model, size, shape)
    jobs = []
    for _ in range(args.n):
        size = rng.choices(sizes, weights=probs, k=1)[0]
        use_bw = (rng.random() < args.bw_frac) and (size in bw_sizes) \
            and (size <= args.bw_max_size)
        model = "bw" if use_bw else "lat"
        shapes = (bw_sizes if model == "bw" else lat_sizes)[size]
        shape = rng.choice(shapes)
        target = (bw_dir if model == "bw" else lat_dir)[shape][0]
        jobs.append((model, size, shape, target))

    # arrival times
    times = []
    t = 0.0
    if args.arrival_dist == "poisson":
        for _ in range(args.n):
            times.append(t)
            t += rng.expovariate(1.0 / args.mean_ia_ns)
    else:  # bursty
        i = 0
        while i < args.n:
            k = min(args.burst_size, args.n - i)
            for _ in range(k):
                times.append(t)
            i += k
            t += args.burst_size * args.mean_ia_ns
    times = [int(round(x)) for x in times]

    order = sorted(range(args.n), key=lambda i: times[i])
    os.makedirs(os.path.join(args.out, "jobs"), exist_ok=True)
    jobs_dir = os.path.join(args.out, "jobs")
    for nm in os.listdir(jobs_dir):
        p = os.path.join(jobs_dir, nm)
        if os.path.islink(p):
            os.unlink(p)

    with open(os.path.join(args.out, "arrivals.csv"), "w", newline="") as f, \
         open(os.path.join(args.out, "job_models.csv"), "w", newline="") as mf:
        w = csv.writer(f)
        mw = csv.writer(mf)
        w.writerow(["job_id", "arrival_time_ns", "num_ranks", "shape"])
        mw.writerow(["job_id", "model", "num_ranks", "shape"])
        for jid, oi in enumerate(order):
            model, size, shape, target = jobs[oi]
            w.writerow([jid, times[oi], size, shape])
            mw.writerow([jid, model, size, shape])
            os.symlink(target, os.path.join(jobs_dir, str(jid)))

    nbw = sum(1 for m, *_ in jobs if m == "bw")
    with open(os.path.join(args.out, "trace_config.txt"), "w") as f:
        for k, v in sorted(vars(args).items()):
            f.write(f"{k}={v}\n")
        f.write(f"actual_bw_jobs={nbw}\nactual_lat_jobs={args.n - nbw}\n")
    print(f"wrote {args.out}: {args.n} jobs ({nbw} bw, {args.n - nbw} lat), "
          f"{len(set(s for _, _, s, _ in jobs))} distinct shapes")


if __name__ == "__main__":
    main()
