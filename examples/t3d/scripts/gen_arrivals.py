"""Arrival-process trace generator for the load-sweep experiments. The torus
dims come from cluster.json (or --cluster-dims).

Produces a runnable arrivals.csv whose offered load matches a target.

LOAD MODEL
----------
Offered load follows the standard convention (cf. traces-for-16x16x16/scripts/
calibrate.py):

    rho = W / (C * T)

  C = cluster capacity in NPUs (product of the torus dims)
  T = arrival span (ns)
  W = sum_i  num_ranks_i * svc_per_iter(shape_i) * N_i     (NPU*ns)

svc_per_iter(shape) is the MEASURED, isolated, single-iteration service time
(ns) for that shape (produced by measure_svc.py / service_times.csv); the
generator never estimates timing analytically. N_i is the job's duration in
iterations, drawn from a heavy-tailed distribution (see sample_duration).

Given the drawn job sequence (hence a concrete W), the target load inverts to a
mean inter-arrival time:

    mean_ia = W / (C * rho * (n_jobs - 1))

Arrivals form a Poisson process: n_jobs-1 i.i.d. Exponential(mean_ia) gaps,
cumulatively summed, job 0 at t=0.

RESIDENCY ASSUMPTION
--------------------
W is computed as if the simulator holds each job's ranks for all N iterations.
We assume ASTRA-sim implements workload looping later (re-issuing the Chakra DAG
N times before releasing ranks), so actual rho ~ target rho. The num_iterations
column is emitted so the simulator can honor this. Until looping lands the
simulator runs each job for a single iteration, so realized busy time is
svc (not svc*N) and target rho overshoots actual rho by ~mean(N); once looping
is implemented the two converge.

SAMPLING
--------
- size:  truncated Pareto over the sorted DISTINCT-SIZE INDEX (alpha default
         0.5); weights by rank, not value, so the large-job tail survives.
- shape: uniform among legal shapes of the drawn size.
- duration: clamped log-normal over [1, max-iters] (iter-median default 1.3,
         iter-sigma default 1.2). Matches the body of published ML-cluster
         runtime traces: median pins ~55% of jobs at 1 iteration (~71% at <=2),
         sigma dials the tail. The 1..max-iters cap bounds the realized tail, so
         the trace's running-time spread is N * svc_per_iter(shape) -- in this
         cluster svc is ~flat over the sampled (small-size) region, so N carries
         most of it.

DETERMINISM
-----------
The entire job sequence is drawn from a single seeded RNG BEFORE any arrival
draw; arrival times scale linearly in mean_ia. Same seed => identical trace.

See docs/superpowers/specs/2026-06-25-cluster4096-arrival-process-design.md.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import random
from collections.abc import Callable

import cluster
import shapes


def legal_shapes(
    model: str,
    size_min: int | None = None,
    size_max: int | None = None,
    dims: frozenset[int] | None = None,
) -> list[tuple[int, int, int]]:
    """Legal shapes for the model, in all_legal_shapes order (sorted by
    (size, shape)), optionally restricted to shapes whose size is in
    [size_min, size_max] and whose every dimension is in `dims`.

    `dims` models trace-characteristic experiments that carve a sub-lattice out
    of the geometric legal set (e.g. dims={1,4,8} = block-aligned shapes). It
    never adds shapes outside the tracelib, so the existing trace library is
    reused verbatim."""
    out: list[tuple[int, int, int]] = []
    for s in shapes.all_legal_shapes(model):
        if dims is not None and not all(d in dims for d in s):
            continue
        size = s[0] * s[1] * s[2]
        if size_min is not None and size < size_min:
            continue
        if size_max is not None and size > size_max:
            continue
        out.append(s)
    if not out:
        raise ValueError(
            "no legal shapes in the requested [size_min, size_max]/dims range"
        )
    return out


def legal_sizes(
    model: str,
    size_min: int | None = None,
    size_max: int | None = None,
    dims: frozenset[int] | None = None,
) -> list[int]:
    """Distinct legal job sizes (rank counts), sorted ascending, optionally
    trimmed to [size_min, size_max] and restricted to shapes over `dims`."""
    return sorted(
        {a * b * c for (a, b, c) in legal_shapes(model, size_min, size_max, dims)}
    )


def sample_size_index(rng: random.Random, m: int, alpha: float) -> int:
    """Draw an index in [0, m-1] from a continuous truncated Pareto over the
    index domain [1, m], then floor.

    F(x) = (1 - x**-alpha) / (1 - m**-alpha),  x in [1, m]
    inverse-CDF: x = (1 - U*(1 - m**-alpha))**(-1/alpha)
    """
    u = rng.random()
    x = (1.0 - u * (1.0 - m ** (-alpha))) ** (-1.0 / alpha)
    k = int(math.floor(x)) - 1
    return min(max(k, 0), m - 1)


def sample_duration(
    rng: random.Random,
    max_iters: int = 20,
    median: float = 1.3,
    sigma: float = 1.2,
) -> int:
    """Job duration in iterations: a discretized, clamped log-normal over
    [1, max_iters] -- heavy-tailed (most jobs 1-2 iters, a thin tail).

    Log-normal (log(N) ~ Normal(ln median, sigma)) matches the body of published
    ML-cluster runtime traces and gives two decoupled knobs: `median` pins how
    much mass sits at 1-2 (median 1.3 => ~55% at N=1, ~71% at N<=2), while
    `sigma` alone dials tail heaviness (p99/p50 ~ exp(2.33*sigma) before
    clamping). All moments are finite, so mean N (the sim-cost proxy) stays low
    even with a real tail.

    The [1, max_iters] clamp caps the realized tail: a heavier sigma piles mass
    at max_iters rather than extending past it. The running-time tail of the
    trace is N * svc_per_iter(shape); in this cluster svc is roughly flat over
    the sampled (small-size) region, so N carries most of the spread.

        x = round(exp(Normal(ln median, sigma))),  clamped to [1, max_iters]
    """
    if max_iters < 1:
        raise ValueError("max_iters must be >= 1")
    if max_iters == 1:
        return 1
    if median <= 0:
        raise ValueError("median must be > 0")
    if sigma <= 0:
        raise ValueError("sigma must be > 0")
    x = round(math.exp(rng.gauss(math.log(median), sigma)))
    return min(max(int(x), 1), max_iters)


def build_job_sequence(
    rng: random.Random,
    n: int,
    model: str,
    alpha: float,
    size_min: int | None = None,
    size_max: int | None = None,
    max_iters: int = 20,
    iter_median: float = 1.3,
    iter_sigma: float = 1.2,
    size_dist: str = "pareto",
    dims: frozenset[int] | None = None,
) -> list[tuple[int, tuple[int, int, int], int]]:
    """Draw n jobs as (size, shape, num_iterations).

    Size is drawn over the sorted DISTINCT-SIZE INDEX: `size_dist="pareto"`
    (truncated Pareto, small-size-favoring, exponent alpha) or "uniform" (each
    distinct legal size equally likely). Shape ~ uniform among the legal shapes
    of that size (restricted to `dims` when given). Duration ~ clamped log-normal
    over [1, max_iters] (sample_duration, params iter_median / iter_sigma).

    Both index draws consume exactly one rng call, and shapes-of-size are taken
    in all_legal_shapes order, so the default (pareto, dims=None) path is
    byte-identical to the original two-line sampler."""
    allowed = legal_shapes(model, size_min, size_max, dims)
    sizes = sorted({a * b * c for (a, b, c) in allowed})
    m = len(sizes)
    by_size: dict[int, list[tuple[int, int, int]]] = {}
    for s in allowed:
        by_size.setdefault(s[0] * s[1] * s[2], []).append(s)
    jobs: list[tuple[int, tuple[int, int, int], int]] = []
    for _ in range(n):
        if size_dist == "uniform":
            idx = rng.randrange(m)
        else:
            idx = sample_size_index(rng, m, alpha)
        size = sizes[idx]
        shape = rng.choice(by_size[size])
        jobs.append(
            (size, shape, sample_duration(rng, max_iters, iter_median, iter_sigma))
        )
    return jobs


def load_service_times(path: str) -> dict[str, float]:
    """Read service_times.csv -> {shape_str: svc_per_iter_ns}."""
    table: dict[str, float] = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            table[row["shape"]] = float(row["svc_per_iter_ns"])
    return table


def make_svc_fn(
    svc_table: dict[str, float] | None,
    uniform_svc_ns: float | None,
) -> Callable[[tuple[int, int, int]], float]:
    """Return shape -> svc_per_iter_ns. Exactly one source must be given."""
    if uniform_svc_ns is not None:
        value = float(uniform_svc_ns)
        return lambda _shape: value
    if svc_table is None:
        raise ValueError("need either a service-time table or uniform_svc_ns")

    def lookup(shape: tuple[int, int, int]) -> float:
        key = shapes.fmt_shape(shape)
        if key not in svc_table:
            raise KeyError(f"shape {key} missing from service-time table")
        return svc_table[key]

    return lookup


def compute_work(
    jobs: list[tuple[int, tuple[int, int, int], int]],
    svc_of: Callable[[tuple[int, int, int]], float],
) -> float:
    """Offered work = sum of (size * svc_per_iter(shape) * num_iters)."""
    return sum(size * svc_of(shape) * iters for size, shape, iters in jobs)


def mean_ia_ns(
    work: float,
    rho: float,
    n_jobs: int,
    capacity: int,
) -> float:
    """Invert target offered load rho = W / (C * T) to a mean inter-arrival
    time, with T approximated as (n_jobs - 1) * mean_ia."""
    if rho <= 0:
        raise ValueError("rho must be > 0")
    if n_jobs < 2:
        raise ValueError("need at least 2 jobs")
    return work / (capacity * rho * (n_jobs - 1))


def draw_arrivals(rng: random.Random, n_jobs: int, mean_ia: float) -> list[int]:
    """Draw n_jobs arrival times in nanoseconds, with job 0 at t=0 and
    inter-arrival times ~ Exponential(mean_ia). Returns a sorted list of
    cumulative arrival times."""
    arrivals: list[int] = [0]
    t = 0.0
    for _ in range(n_jobs - 1):
        ia = -math.log(1.0 - rng.random()) * mean_ia
        t += ia
        arrivals.append(int(round(t)))
    return arrivals


def write_outputs(
    out_dir: str,
    jobs: list[tuple[int, tuple[int, int, int], int]],
    times: list[int],
    args: argparse.Namespace,
    work: float,
    mean_ia: float,
    model: str,
    traces: str | None = None,
) -> None:
    """Write arrivals.csv, trace_config.txt, and (if traces given) jobs/<id>
    symlinks into <traces>/<model>/<SHAPE>."""
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "arrivals.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            ["job_id", "arrival_time_ns", "num_ranks", "shape", "num_iterations"]
        )
        for jid, ((size, shape, iters), t) in enumerate(zip(jobs, times)):
            w.writerow([jid, t, size, shapes.fmt_shape(shape), iters])

    with open(os.path.join(out_dir, "trace_config.txt"), "w") as f:
        for k, v in sorted(vars(args).items()):
            f.write(f"{k}={v}\n")
        f.write(f"realized_work_npu_ns={work}\n")
        f.write(f"mean_ia_ns={mean_ia}\n")

    if traces:
        jobs_dir = os.path.join(out_dir, "jobs")
        os.makedirs(jobs_dir, exist_ok=True)
        for jid, (_size, shape, _iters) in enumerate(jobs):
            target = os.path.join(
                os.path.abspath(traces), model, shapes.fmt_shape(shape)
            )
            link = os.path.join(jobs_dir, str(jid))
            if os.path.islink(link) or os.path.exists(link):
                os.unlink(link)
            os.symlink(target, link)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="t3d arrival-process generator")
    ap.add_argument("--rho", type=float, required=True, help="target offered load")
    ap.add_argument("--out", required=True, help="output cell directory")
    ap.add_argument("--n", type=int, default=500, help="number of jobs")
    ap.add_argument("--model", default="bw", choices=shapes.MODELS)
    ap.add_argument("--svc", default=None, help="service_times.csv (measured)")
    ap.add_argument(
        "--uniform-svc-ns",
        type=float,
        default=None,
        help="dev fallback: constant svc_per_iter for every shape",
    )
    ap.add_argument("--alpha", type=float, default=0.5, help="size-Pareto exponent")
    ap.add_argument(
        "--size-dist",
        choices=("pareto", "uniform"),
        default="pareto",
        help="size distribution over the distinct-size index (uniform ignores --alpha)",
    )
    ap.add_argument(
        "--dims",
        default=None,
        help="restrict shapes to those whose every dimension is in this comma "
        "list, e.g. '1,4,8' for block-aligned shapes (default: all legal dims)",
    )
    ap.add_argument(
        "--max-iters",
        type=int,
        default=20,
        help="duration cap; iterations are drawn from [1, max-iters]",
    )
    ap.add_argument(
        "--iter-median",
        type=float,
        default=1.3,
        help="log-normal duration median (pins mass at 1-2 iters)",
    )
    ap.add_argument(
        "--iter-sigma",
        type=float,
        default=1.2,
        help="log-normal duration sigma (larger = heavier tail, costlier sims)",
    )
    ap.add_argument("--size-min", type=int, default=None)
    ap.add_argument("--size-max", type=int, default=None)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument(
        "--traces", default=None, help="trace library root for jobs/ symlinks"
    )
    ap.add_argument(
        "--cluster-dims",
        default=None,
        help="torus dims AxBxC (default: cluster.json in the example root)",
    )
    args = ap.parse_args(argv)

    if (args.svc is None) == (args.uniform_svc_ns is None):
        ap.error("exactly one of --svc or --uniform-svc-ns is required")

    cluster_dims = (
        cluster.parse_dims(args.cluster_dims)
        if args.cluster_dims
        else cluster.load(cluster.default_root())
    )
    shapes.init(cluster_dims)
    dims = frozenset(int(x) for x in args.dims.split(",")) if args.dims else None

    rng = random.Random(args.seed)
    jobs = build_job_sequence(
        rng,
        args.n,
        args.model,
        args.alpha,
        args.size_min,
        args.size_max,
        args.max_iters,
        args.iter_median,
        args.iter_sigma,
        args.size_dist,
        dims,
    )
    svc_table = load_service_times(args.svc) if args.svc else None
    svc_of = make_svc_fn(svc_table, args.uniform_svc_ns)
    work = compute_work(jobs, svc_of)
    mean_ia = mean_ia_ns(work, args.rho, args.n, cluster.capacity(cluster_dims))
    times = draw_arrivals(rng, args.n, mean_ia)
    write_outputs(args.out, jobs, times, args, work, mean_ia, args.model, args.traces)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
