"""Measure isolated single-iteration service time per legal shape.

For each legal shape that has a Chakra trace, run it ALONE on the idle torus
(dims from cluster.json or --cluster-dims) through the reconfigurable
analytical binary (fifo admission, firstfit
placement) and record the single job's JCT as svc_per_iter. Writes
service_times.csv (shape,size,svc_per_iter_ns), consumed by gen_arrivals.py.

This is the single-model t3d analogue of
traces-for-16x16x16/scripts/calibrate.py `measure`.

    python3 measure_svc.py --traces <lib> --out service_times.csv \
        --cfg ../configs \
        --astra-sim <project>/build/astra_analytical/build/bin/AstraSim_Analytical_Reconfigurable
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import os
import resource
import shutil
import subprocess
import tempfile

import cluster
import shapes

SIM_TIMEOUT_S: int = 1200


def shape_trace_dir(traces: str, model: str, shape: tuple[int, int, int]) -> str:
    return os.path.join(os.path.abspath(traces), model, shapes.fmt_shape(shape))


def has_trace(traces: str, model: str, shape: tuple[int, int, int]) -> bool:
    return os.path.isfile(
        os.path.join(shape_trace_dir(traces, model, shape), "chakra_trace.0.et")
    )


def legal_shapes_with_traces(
    model: str,
    traces: str,
    only: list[str] | None = None,
    extra_file: str | None = None,
) -> list[tuple[int, int, int]]:
    """Legal shapes (optionally restricted to `only`) that have a trace present.
    `extra_file` unions in the shapes it lists, one AxBxC per line (mirrors
    gen_traces --extra-shapes-file)."""
    if only:
        candidates = [shapes.parse_shape(s) for s in only]
    else:
        candidates = shapes.all_legal_shapes(model)
        if extra_file:
            with open(extra_file) as f:
                listed = {shapes.parse_shape(ln.strip()) for ln in f if ln.strip()}
            candidates = sorted(
                set(candidates) | listed, key=lambda t: (t[0] * t[1] * t[2], t)
            )
    return [s for s in candidates if has_trace(traces, model, s)]


def write_single_job_dir(
    work_dir: str,
    shape: tuple[int, int, int],
    traces: str,
    model: str,
) -> int:
    """Write a 4-column 1-job arrivals.csv (current-binary format) + jobs/0
    symlink to the shape's trace dir. Returns the job size."""
    size = shape[0] * shape[1] * shape[2]
    with open(os.path.join(work_dir, "arrivals.csv"), "w", newline="") as f:
        w = csv.writer(f)
        # num_iterations=1: the binary replays each job's trace num_iterations
        # times, so a single iteration yields svc_per_iter directly.
        w.writerow(
            ["job_id", "arrival_time_ns", "num_ranks", "shape", "num_iterations"]
        )
        w.writerow([0, 0, size, shapes.fmt_shape(shape), 1])
    jobs_dir = os.path.join(work_dir, "jobs")
    os.makedirs(jobs_dir, exist_ok=True)
    link = os.path.join(jobs_dir, "0")
    if os.path.islink(link) or os.path.exists(link):
        os.unlink(link)
    os.symlink(shape_trace_dir(traces, model, shape), link)
    return size


def npus_dims(npus: str) -> tuple[int, ...]:
    """The torus dims behind an '--npus-per-dim=A,B,C' value."""
    return tuple(int(x) for x in npus.split(","))


def run_one(
    shape: tuple[int, int, int],
    traces: str,
    model: str,
    cfg: str,
    astra_sim: str,
    npus: str,
) -> tuple[tuple[int, int, int], int, int | None, str | None]:
    """Run one isolated job. Returns (shape, size, svc_per_iter_ns, error)."""
    d = tempfile.mkdtemp(prefix="svc_")
    try:
        size = write_single_job_dir(d, shape, traces, model)
        out = os.path.join(d, "output")
        os.makedirs(out, exist_ok=True)
        cmd = [
            astra_sim,
            f"--system-configuration={cfg}/system.json",
            f"--remote-memory-configuration={cfg}/remote_memory.json",
            f"--network-configuration={cfg}/network.yml",
            f"--bw-schedule={cfg}/bandwidth_schedule_torus.txt",
            f"--latency-schedule={cfg}/latency_schedule_torus.txt",
            f"--logging-folder={out}",
            "--num-queues-per-dim=1",
            "--comm-scale=1.0",
            "--injection-scale=1.0",
            "--rendezvous-protocol=false",
            f"--npus-per-dim={npus}",
            f"--job-arrival-file={os.path.join(d, 'arrivals.csv')}",
            f"--jobs-dir={os.path.join(d, 'jobs')}",
            # firstfit needs the shape to fit the torus; expanded-universe
            # shapes (dims beyond an axis) are placed by rfold at its default
            # block instead -- exactly the sweep arm they run under, and the
            # donly universe is pre-filtered to what it can place
            # (placeability.rfold_placeable_file)
            *(
                ["--placement-policy=firstfit"]
                if all(d <= n for d, n in zip(shape, npus_dims(npus)))
                else ["--placement-policy=rfold"]
            ),
            "--admission-policy=fifo",
        ]
        try:
            r = subprocess.run(
                cmd, capture_output=True, text=True, timeout=SIM_TIMEOUT_S
            )
        except subprocess.TimeoutExpired:
            return (shape, size, None, f"sim timeout after {SIM_TIMEOUT_S}s")
        if r.returncode != 0:
            # the binary logs fatal errors to stdout (spdlog), not stderr
            err = (r.stderr.strip() or r.stdout.strip())[-200:]
            return (shape, size, None, f"sim failed: {err}")
        jobs_csv = os.path.join(out, "jobs.csv")
        if not os.path.isfile(jobs_csv):
            return (shape, size, None, "no jobs.csv produced")
        with open(jobs_csv, newline="") as f:
            rows = list(csv.DictReader(f))
        if not rows:
            return (shape, size, None, "job did not complete")
        if int(rows[0]["queue_wait_ns"]) != 0:
            return (shape, size, None, "nonzero queue wait in isolation")
        return (shape, size, int(rows[0]["jct_ns"]), None)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def write_table(
    out: str,
    rows: list[tuple[tuple[int, int, int], int, int]],
) -> None:
    """Write service_times.csv, sorted by (size, shape)."""
    ordered = sorted(rows, key=lambda r: (r[1], r[0]))
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["shape", "size", "svc_per_iter_ns"])
        for shape, size, svc in ordered:
            w.writerow([shapes.fmt_shape(shape), size, svc])


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="per-shape service-time measurer")
    ap.add_argument("--traces", required=True, help="Chakra trace library root")
    ap.add_argument("--out", required=True, help="service_times.csv output path")
    ap.add_argument("--cfg", required=True, help="configs dir")
    ap.add_argument("--astra-sim", required=True, help="reconfigurable binary path")
    ap.add_argument("--model", default="bw", choices=shapes.MODELS)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument(
        "--only", default=None, help="comma-separated shapes to restrict to"
    )
    ap.add_argument(
        "--extra-shapes-file",
        default=None,
        help="also measure the shapes listed in this file (one AxBxC per "
        "line; mirrors gen_traces --extra-shapes-file)",
    )
    ap.add_argument(
        "--cluster-dims",
        default=None,
        help="torus dims AxBxC (default: cluster.json in the example root)",
    )
    args = ap.parse_args(argv)

    dims = (
        cluster.parse_dims(args.cluster_dims)
        if args.cluster_dims
        else cluster.load(cluster.default_root())
    )
    shapes.init(dims)

    # Each sim opens one .et per rank, so the biggest shape (= the full torus)
    # blows through the usual 1024 soft fd cap. Raise it for us and the sims.
    need = cluster.capacity(dims) + 256
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft < need:
        if hard < need:
            ap.error(f"open-files hard limit {hard} < {need}; raise ulimit -Hn")
        resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))

    only = args.only.split(",") if args.only else None
    todo = legal_shapes_with_traces(
        args.model, args.traces, only, args.extra_shapes_file
    )
    if not todo:
        ap.error("no legal shapes with traces found under --traces")

    cfg = os.path.abspath(args.cfg)
    results: list[tuple[tuple[int, int, int], int, int]] = []
    failures: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        npus = cluster.npus_arg(dims)
        futs = {
            ex.submit(run_one, s, args.traces, args.model, cfg, args.astra_sim, npus): s
            for s in todo
        }
        for fut in concurrent.futures.as_completed(futs):
            shape, size, svc, err = fut.result()
            if err is not None:
                failures.append(f"{shapes.fmt_shape(shape)}: {err}")
            else:
                results.append((shape, size, svc))

    write_table(args.out, results)
    print(f"wrote {args.out}: {len(results)} shapes measured, {len(failures)} failed")
    for f in failures:
        print(f"  FAIL {f}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
