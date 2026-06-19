#!/usr/bin/env python3
"""Offered-load calibration + verification for the 8x8x8 sweep.

Offered load: rho = W / (C * T), with W = sum(size_i * svc_i) in NPU*ns,
C = 512 NPUs, T = arrival span (ns). svc is the *isolated service time* of a
(model, shape) mixlib entry: its JCT run alone on the idle torus (firstfit).

Subcommands:
  measure  one single-job firstfit sim per mixlib entry -> service_times.csv
  ia       exact mean-ia-ns for a cell spec + rho target. Relies on two
           determinism facts of gen_mixed.py (fixed seed): the job sequence
           is drawn before any arrival draw (so it is independent of ia),
           and arrival times scale linearly in ia. Probe-generate at
           PROBE_IA, then ia = W * PROBE_IA / (C * rho * S_probe).
           Prints: "<ia_ns> <W_npu_ns> <unit_span>"
  check    post-run measured load of a finished cell from the firstfit
           run's flat results dir (svc = jct - queue_wait). Exits 0 in
           band, 2 otherwise, and prints a suggested rescaled ia (spec
           section 8).
"""

import argparse
import concurrent.futures
import csv
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CAPACITY = 512
PROBE_IA = 1e6  # ns; large so the int-ns rounding of arrivals is negligible


def lib_entries(lib):
    """[(model, shape, size, abs_dir)] for every complete mixlib entry."""
    out = []
    for model in ("lat", "bw"):
        root = os.path.join(lib, model)
        if not os.path.isdir(root):
            continue
        for shape in sorted(os.listdir(root)):
            d = os.path.join(root, shape)
            if not os.path.isfile(os.path.join(d, "chakra_trace.0.et")):
                continue
            a, b, c = (int(x) for x in shape.split("x"))
            out.append((model, shape, a * b * c, os.path.abspath(d)))
    return out


def read_service(path):
    with open(path, newline="") as f:
        return {(r["model"], r["shape"]): int(r["svc_ns"]) for r in csv.DictReader(f)}


def job_work(cell_dir, svc):
    """W = sum(size * isolated svc) over the cell's job_models.csv (NPU*ns)."""
    w = 0
    with open(os.path.join(cell_dir, "job_models.csv"), newline="") as f:
        for r in csv.DictReader(f):
            w += int(r["num_ranks"]) * svc[(r["model"], r["shape"])]
    return w


def arrival_span(cell_dir):
    with open(os.path.join(cell_dir, "arrivals.csv"), newline="") as f:
        times = [int(r["arrival_time_ns"]) for r in csv.DictReader(f)]
    return max(times) - min(times)


# ---------------------------------------------------------------- measure --
def run_one(entry, runs_dir):
    model, shape, size, target = entry
    d = os.path.join(runs_dir, f"{model}_{shape}")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "jobs"))
    with open(os.path.join(d, "arrivals.csv"), "w") as f:
        f.write("job_id,arrival_time_ns,num_ranks,shape\n")
        f.write(f"0,0,{size},{shape}\n")
    os.symlink(target, os.path.join(d, "jobs", "0"))
    try:
        r = subprocess.run(
            ["bash", os.path.join(HERE, "run_policy.sh"), d, "fifo", "firstfit"],
            capture_output=True,
            text=True,
            timeout=1800,
        )
    except subprocess.TimeoutExpired:
        return (model, shape, size, None, "sim timeout after 1800s")
    if r.returncode != 0:
        return (model, shape, size, None, f"sim failed: {r.stderr.strip()[-200:]}")
    out_csv = os.path.join(d, "output", "fifo", "firstfit", "jobs.csv")
    if not os.path.isfile(out_csv):
        return (model, shape, size, None, "no jobs.csv produced")
    with open(out_csv, newline="") as f:
        rows = list(csv.DictReader(f))
    if len(rows) != 1 or rows[0]["status"] != "COMPLETED":
        return (model, shape, size, None, "job did not complete")
    if int(rows[0]["queue_wait_ns"]) != 0:
        return (model, shape, size, None, "nonzero queue wait in isolation")
    return (model, shape, size, int(rows[0]["jct_ns"]), None)


def cmd_measure(args):
    entries = lib_entries(args.lib)
    if not entries:
        sys.exit(f"no library entries under {args.lib}")
    runs_dir = os.path.join(os.path.dirname(os.path.abspath(args.out)), "runs")
    os.makedirs(runs_dir, exist_ok=True)
    results, errs = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.maxpar) as ex:
        for model, shape, size, svc, err in ex.map(
            lambda e: run_one(e, runs_dir), entries
        ):
            if err:
                errs.append(f"{model}/{shape}: {err}")
                print(f"FAIL {model}/{shape}: {err}", file=sys.stderr)
            else:
                results.append((model, shape, size, svc))
                print(f"ok {model}/{shape} size={size} svc={svc}ns", flush=True)
    if errs:
        sys.exit(f"{len(errs)}/{len(entries)} calibration sims failed -- aborting")
    results.sort()
    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["model", "shape", "size", "svc_ns"])
        w.writerows(results)
    print(f"wrote {args.out}: {len(results)} entries")


# --------------------------------------------------------------------- ia --
def cmd_ia(args):
    svc = read_service(args.svc)
    have = {(m, s) for (m, s) in svc}
    by_model = {}
    for model, shape, size, _ in lib_entries(args.lib):
        by_model.setdefault(model, set()).add(size)
        if (model, shape) not in have:
            sys.exit(f"no service time for {model}/{shape} in {args.svc}")
    weight_sizes = [int(t.split(":")[0]) for t in args.size_weights.split(",")]
    for s in weight_sizes:
        for model in ("lat", "bw"):
            if s not in by_model.get(model, set()):
                sys.exit(
                    f"size {s} missing from {model} library -- gen_mixed "
                    "would silently drop it"
                )
    tmp = tempfile.mkdtemp(prefix="calib_ia_")
    try:
        try:
            subprocess.run(
                [
                    sys.executable,
                    os.path.join(HERE, "gen_mixed.py"),
                    "--out",
                    tmp,
                    "--lib",
                    args.lib,
                    "--n",
                    str(args.n),
                    "--seed",
                    str(args.seed),
                    "--bw-frac",
                    str(args.bw_frac),
                    "--bw-max-size",
                    str(args.bw_max_size),
                    "--size-weights",
                    args.size_weights,
                    "--mean-ia-ns",
                    str(PROBE_IA),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
        except subprocess.CalledProcessError as e:
            sys.exit(f"gen_mixed probe failed: {e.stderr.strip()[-500:]}")
        w = job_work(tmp, svc)
        s_probe = arrival_span(tmp)
        unit_span = s_probe / PROBE_IA
        ia = w / (CAPACITY * args.rho * unit_span)
        print(f"{ia:.6f} {w} {unit_span:.9f}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ------------------------------------------------------------------ check --
def cmd_check(args):
    out = os.path.join(args.results, "jobs.csv")
    if not os.path.isfile(out):
        sys.exit(f"missing {out}")
    work, arr, bad = 0, [], 0
    with open(out, newline="") as f:
        for r in csv.DictReader(f):
            if r["status"] != "COMPLETED":
                bad += 1
                continue
            arr.append(int(r["arrival_time_ns"]))
            work += int(r["num_ranks"]) * (int(r["jct_ns"]) - int(r["queue_wait_ns"]))
    if bad:
        sys.exit(f"{args.cell_dir}: {bad} jobs not COMPLETED")
    span = max(arr) - min(arr)
    rho = work / (CAPACITY * span)
    lo, hi = args.target - args.band, args.target + args.band
    verdict = "OK" if lo <= rho <= hi else ("HIGH" if rho > hi else "LOW")
    cur_ia = None
    with open(os.path.join(args.cell_dir, "trace_config.txt")) as f:
        for line in f:
            if line.startswith("mean_ia_ns="):
                cur_ia = float(line.split("=", 1)[1])
    suggested = cur_ia * rho / args.target if cur_ia else 0.0
    print(
        f"rho_measured={rho:.4f} target={args.target} band=[{lo:.2f},{hi:.2f}] "
        f"verdict={verdict} suggested_ia={suggested:.6f}"
    )
    sys.exit(0 if verdict == "OK" else 2)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("measure")
    m.add_argument("--lib", required=True)
    m.add_argument("--out", required=True)
    m.add_argument("--maxpar", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    m.set_defaults(fn=cmd_measure)

    i = sub.add_parser("ia")
    i.add_argument("--lib", required=True)
    i.add_argument("--svc", required=True)
    i.add_argument("--bw-frac", required=True)
    i.add_argument("--size-weights", required=True)
    i.add_argument("--rho", type=float, required=True)
    i.add_argument("--n", type=int, default=400)
    i.add_argument("--seed", type=int, default=1)
    i.add_argument("--bw-max-size", type=int, default=9999)
    i.set_defaults(fn=cmd_ia)

    c = sub.add_parser("check")
    c.add_argument("cell_dir", help="trace dir (for trace_config.txt)")
    c.add_argument("--results", required=True, help="run dir holding jobs.csv")
    c.add_argument("--target", type=float, required=True)
    c.add_argument("--band", type=float, default=0.05)
    c.set_defaults(fn=cmd_check)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
