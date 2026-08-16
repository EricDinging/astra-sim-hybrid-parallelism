"""Single-job placeability census over an empty torus.

For every even-or-1 shape AxBxC with prod <= capacity, probe whether ONE
job of that shape can be placed on an EMPTY torus under FIFO admission,
for firstfit plus rfold at every power-of-two cubic block size that tiles
the torus (1x1x1, 2x2x2, ... up to the torus itself = folding-only).
The fail<pct> experiment variants mark ceil(pct)% of the NPUs permanently
failed (binary seed 42, same convention as the fail load-sweeps).

No JCT is measured, so no traces are needed: the jobs dir is left EMPTY.
A shape the policy places dies immediately at workload load with a
distinctive "not readable" critical log; an unplaceable one logs DROPPED
and exits 0. Either way a probe is ~0.1 s, so the whole census runs
locally. Results land in runs/<exp>/placeability.csv.
"""

from __future__ import annotations

import concurrent.futures
import csv
import os
import subprocess
import tempfile

import cluster
import run_combo  # failure_flags

FAIL_PCTS = ["0.1", "0.2", "0.5", "1"]
EXPS = ["placeability", *(f"placeability-fail{p}" for p in FAIL_PCTS)]
CSV_NAME = "placeability.csv"


def shapes(cap: int) -> list[tuple[int, int, int]]:
    """All shapes with each dim 1-or-even, prod <= cap, deduped by sorting
    each shape A <= B <= C. Wider than shapes.DIM_DOMAIN on purpose: a dim
    may exceed the torus axis (1x1x512 is a legal shape that merely cannot
    fit an 8x8x8)."""
    vals = [1, *range(2, cap + 1, 2)]
    return [
        (a, b, c)
        for a in vals
        for b in vals
        if b >= a and a * b <= cap
        for c in vals
        if c >= b and a * b * c <= cap
    ]


def ndim(shape: tuple[int, int, int]) -> int:
    """Dimensionality of a shape = dims > 1 (1x1x1 counts as 1D)."""
    return max(1, sum(d > 1 for d in shape))


def policies(dims: tuple[int, int, int]) -> list[tuple[str, list[str]]]:
    """firstfit + rfold at each power-of-two cubic block that tiles the
    torus; the largest (the torus itself, on a cubic torus) is folding-only."""
    out: list[tuple[str, list[str]]] = [("firstfit", ["--placement-policy=firstfit"])]
    b = 1
    while all(d % b == 0 for d in dims):
        out.append(
            (
                f"rfold@{b}x{b}x{b}",
                ["--placement-policy=rfold", f"--block-size={b}x{b}x{b}"],
            )
        )
        b *= 2
    return out


def probe(
    binary: str,
    cfg: str,
    dims_csv: str,
    shape: tuple[int, int, int],
    pol_flags: list[str],
    extra: list[str],
) -> str:
    """One placement attempt -> placed | dropped | defer | timeout | error."""
    a, b, c = shape
    with tempfile.TemporaryDirectory(prefix="placeprobe_") as td:
        jobs = os.path.join(td, "jobs")
        os.makedirs(jobs)  # intentionally left empty (see module docstring)
        arrivals = os.path.join(td, "arrivals.csv")
        with open(arrivals, "w") as f:
            f.write("job_id,arrival_time_ns,num_ranks,shape,num_iterations\n")
            f.write(f"0,0,{a * b * c},{a}x{b}x{c},1\n")
        try:
            p = subprocess.run(
                [
                    binary,
                    f"--system-configuration={cfg}/system.json",
                    f"--remote-memory-configuration={cfg}/remote_memory.json",
                    f"--network-configuration={cfg}/network.yml",
                    f"--bw-schedule={cfg}/bandwidth_schedule_torus.txt",
                    f"--latency-schedule={cfg}/latency_schedule_torus.txt",
                    f"--logging-folder={td}",
                    "--num-queues-per-dim=1",
                    "--comm-scale=1.0",
                    "--injection-scale=1.0",
                    "--rendezvous-protocol=false",
                    f"--npus-per-dim={dims_csv}",
                    f"--job-arrival-file={arrivals}",
                    f"--jobs-dir={jobs}",
                    "--admission-policy=fifo",
                    *pol_flags,
                    *extra,
                ],
                capture_output=True,
                text=True,
                timeout=900,
            )
        except subprocess.TimeoutExpired:
            return "timeout"
    out = p.stdout + p.stderr
    if "not readable" in out:  # died loading the (absent) trace => was placed
        return "placed"
    if "DROPPED" in out:
        return "dropped"
    # a shape rfold's idle-torus oracle misses defers forever; the drained
    # event queue then trips SchedRuntime's deadlock assert (SIGABRT)
    if p.returncode == 0 or "drained with jobs incomplete" in out:
        return "defer"
    return f"error rc={p.returncode}"


def rfold_placeable_file(root: str, binary: str, cap: int) -> str:
    """Probe every expanded-universe shape (shapes.expanded_legal_shapes, cap
    = the donly sweeps' size cap) for placeability under rfold at its DEFAULT
    block size on an idle torus, and cache the placed subset one AxBxC per
    line in <root>/rfold_placeable<cap>.txt (delete the file to re-probe).

    That file is the <nd>donly sampling universe: every arm those sweeps run
    (rfold, scatter policies, ideal) can place 100% of a trace drawn from it,
    so JCT is compared on identical fully-placed workloads. Probes need no
    traces (see probe())."""
    import shapes as shp

    path = os.path.join(root, f"rfold_placeable{cap}.txt")
    if os.path.isfile(path):
        print(f"skip  {path} (exists; delete it to re-probe)")
        return path
    if not os.path.isfile(binary):
        raise SystemExit(f"binary not found: {binary} (run build.sh first)")
    dims = cluster.load(root)
    dims_csv = ",".join(str(d) for d in dims)
    cfg = os.path.join(root, "configs")
    cands = shp.expanded_legal_shapes("bw", cap)
    print(f"probing rfold(default-block) placeability of {len(cands)} shapes ...")
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        outcomes = list(
            ex.map(
                lambda s: probe(
                    binary, cfg, dims_csv, s, ["--placement-policy=rfold"], []
                ),
                cands,
            )
        )
    placed = [s for s, o in zip(cands, outcomes) if o == "placed"]
    odd = {o for o in outcomes if o not in ("placed", "dropped")}
    if odd:
        raise SystemExit(f"unexpected probe outcomes {odd} -- refusing to cache")
    with open(path, "w") as f:
        for s in placed:
            f.write(f"{s[0]}x{s[1]}x{s[2]}\n")
    by_nd_placed = {d: sum(1 for s in placed if ndim(s) == d) for d in (1, 2, 3)}
    by_nd_all = {d: sum(1 for s in cands if ndim(s) == d) for d in (1, 2, 3)}
    print(
        f"wrote {path}: {len(placed)}/{len(cands)} placeable "
        + " ".join(f"{d}D={by_nd_placed[d]}/{by_nd_all[d]}" for d in (1, 2, 3))
    )
    return path


def run(exp: str, root: str, binary: str) -> None:
    """The whole census for one experiment, locally in parallel. Idempotent:
    a complete placeability.csv is not recomputed."""
    if not os.path.isfile(binary):
        raise SystemExit(f"binary not found: {binary} (run build.sh first)")
    dims = cluster.load(root)
    cap = cluster.capacity(dims)
    dims_csv = ",".join(str(d) for d in dims)
    cfg = os.path.join(root, "configs")
    extra = run_combo.failure_flags(exp, dims_csv)

    todo = [(pol, flags, s) for pol, flags in policies(dims) for s in shapes(cap)]
    out_dir = os.path.join(root, "runs", exp)
    out_csv = os.path.join(out_dir, CSV_NAME)
    if os.path.isfile(out_csv):
        with open(out_csv) as f:
            have = sum(1 for _ in f) - 1
        if have == len(todo):
            print(f"skip  {exp} ({CSV_NAME} complete; delete it to re-run)")
            return

    print(
        f"{exp}: {len(todo)} probes ({len(shapes(cap))} shapes x "
        f"{len(policies(dims))} policies) on {cluster.fmt(dims)}"
    )
    os.makedirs(out_dir, exist_ok=True)
    rows = []
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        results = ex.map(
            lambda t: probe(binary, cfg, dims_csv, t[2], t[1], extra), todo
        )
        for i, ((pol, _flags, s), outcome) in enumerate(zip(todo, results), 1):
            rows.append(
                (pol, f"{s[0]}x{s[1]}x{s[2]}", s[0] * s[1] * s[2], ndim(s), outcome)
            )
            if i % 1000 == 0 or i == len(todo):
                print(f"  {exp}: {i}/{len(todo)}")
    with open(out_csv, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["policy", "shape", "num_ranks", "ndim", "outcome"])
        w.writerows(rows)
    print(f"wrote {out_csv}")
    summarize(exp, root)


def summarize(exp: str, root: str) -> None:
    """Per-policy placed/total from runs/<exp>/placeability.csv."""
    out_csv = os.path.join(root, "runs", exp, CSV_NAME)
    if not os.path.isfile(out_csv):
        raise SystemExit(f"{out_csv} not found -- run launch first")
    placed: dict[str, int] = {}
    total: dict[str, int] = {}
    odd: dict[str, dict[str, int]] = {}  # anything besides placed/dropped
    with open(out_csv, newline="") as f:
        for row in csv.DictReader(f):
            pol = row["policy"]
            total[pol] = total.get(pol, 0) + 1
            if row["outcome"] == "placed":
                placed[pol] = placed.get(pol, 0) + 1
            elif row["outcome"] != "dropped":
                by = odd.setdefault(pol, {})
                by[row["outcome"]] = by.get(row["outcome"], 0) + 1
    print(f"{exp}: placeable shapes per policy")
    for pol in total:
        n, t = placed.get(pol, 0), total[pol]
        extra = "".join(f", {k}={v}" for k, v in sorted(odd.get(pol, {}).items()))
        print(f"  {pol:>14}: {n}/{t} ({100 * n / t:.1f}%){extra}")
