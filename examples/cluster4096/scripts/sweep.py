"""Sweep harness for the cluster4096 load-sweep experiments.

One module per concern is overkill here; this file holds the experiment
table plus one function per phase, called from ../reproduce.py:

  prereq()      build the prerequisites: tracelib + service_times.csv
  gen()         arrival-trace generation (implemented)
  launch()      run the sweep combos            (not implemented yet)
  collect()     completion check / result pull  (not implemented yet)
  postprocess() analysis over collected results (not implemented yet)
  clean()       remove all results (keeps the prerequisites)

Every experiment shares the pre-built Chakra tracelib (one trace per legal
shape). A combo folder runs/<exp>/<admission>-<placement>-load<L>/ is fully
self-contained: its arrivals.csv references tracelib jobs by shape (the
per-job iteration count lives only in the `num_iterations` column, so
differing durations never need extra traces), and later phases drop the
result csv files next to it.
"""

from __future__ import annotations

import concurrent.futures
import datetime
import os
import shutil
import tempfile
import time

import gen_arrivals
import gen_traces
import launcher
import measure_svc
import shapes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # examples/cluster4096
REPO = os.path.dirname(os.path.dirname(ROOT))
ASTRA_SIM_BIN = os.path.join(
    REPO,
    "build",
    "astra_analytical",
    "build",
    "bin",
    "AstraSim_Analytical_Reconfigurable",
)

# trace length; override for quick dev tests, e.g. N_JOBS=20 ./reproduce.py gen ...
N_JOBS = int(os.environ.get("N_JOBS", 100000))
# progress-poll interval while launch blocks; override for dev, e.g. POLL_SECS=10
POLL_SECS = int(os.environ.get("POLL_SECS", 3600))
SEED = 0
LOADS = [f"{i / 100:.2f}" for i in range(5, 101, 5)]  # 0.05 .. 1.00, step 0.05
PLACEMENTS = ["firstfit", "rfold", "sfc", "l1clustering", "topomatch", "random"]

# experiment -> gen_arrivals trace-characteristic flags. The admission policy
# is the experiment-name prefix (easy/swf/fifo); it only matters at launch
# time, so the three *-pareto512-* experiments generate identical
# (seed-pinned) arrivals.
EXPERIMENTS: dict[str, list[str]] = {
    "easy-pareto512-load-sweep": ["--alpha", "0.5", "--size-max", "512"],
    "easy-pareto256-load-sweep": ["--alpha", "0.5", "--size-max", "256"],
    "easy-pareto128-load-sweep": ["--alpha", "0.5", "--size-max", "128"],
    "easy-uniform512-load-sweep": ["--size-dist", "uniform", "--size-max", "512"],
    "swf-pareto512-load-sweep": ["--alpha", "0.5", "--size-max", "512"],
    "fifo-pareto512-load-sweep": ["--alpha", "0.5", "--size-max", "512"],
}


def combos(exp: str) -> list[tuple[str, str]]:
    """All (combo_name, load) cells of an experiment, e.g.
    ("easy-firstfit-load0.15", "0.15"). Admission comes from the exp name."""
    admission = exp.split("-")[0]
    return [
        (f"{admission}-{placement}-load{load}", load)
        for placement in PLACEMENTS
        for load in LOADS
    ]


def prereq(root: str = ROOT, jobs: str | None = None) -> None:
    """Build the shared prerequisites for every experiment: the Chakra
    tracelib (idempotent/resumable -- already-built shapes are skipped) and
    the measured service_times.csv (skipped when present; delete it to force
    a re-measure). Neither is touched by clean()."""
    rc = gen_traces.main(
        ["--out", os.path.join(root, "tracelib")] + (["--jobs", jobs] if jobs else [])
    )
    if rc:
        raise SystemExit(rc)
    svc_table = os.path.join(root, "service_times.csv")
    # a table only counts as present when every legal shape is in it --
    # a partially failed measure (e.g. OOM on the big shapes) is re-run
    expected = len(shapes.all_legal_shapes("bw"))
    have = sum(1 for _ in open(svc_table)) - 1 if os.path.isfile(svc_table) else 0
    if have == expected:
        print(f"skip  {svc_table} ({have} shapes; delete it to re-measure)")
    else:
        if have:
            print(f"re-measuring: {svc_table} has {have}/{expected} shapes")
        svc(root)


def svc(root: str = ROOT) -> None:
    """(Re)measure every shape's isolated service time -> service_times.csv.
    Needs the built reconfigurable binary and the tracelib."""
    if not os.path.isfile(ASTRA_SIM_BIN):
        raise SystemExit(f"binary not found: {ASTRA_SIM_BIN} (run build.sh first)")
    rc = measure_svc.main(
        [
            "--traces",
            os.path.join(root, "tracelib"),
            "--cfg",
            os.path.join(root, "configs"),
            "--astra-sim",
            ASTRA_SIM_BIN,
            "--out",
            os.path.join(root, "service_times.csv"),
        ]
    )
    if rc:
        raise SystemExit(rc)


def gen(exp: str, root: str = ROOT) -> None:
    """Generate arrival traces for every combo of the experiment, directly
    into runs/<exp>/<combo>/ (combos with an arrivals.csv are skipped, so
    re-running is cheap and resumable). All placements at one load get the
    same seed-pinned job stream, for a fair policy comparison."""
    svc_table = os.path.join(root, "service_times.csv")
    if not os.path.isfile(svc_table):
        raise SystemExit(
            f"{svc_table} not found -- generate it with ./reproduce.py prereq"
        )
    flags = EXPERIMENTS[exp]
    for combo, load in combos(exp):
        out = os.path.join(root, "runs", exp, combo)
        if os.path.isfile(os.path.join(out, "arrivals.csv")):
            print(f"skip  {exp}/{combo} (arrivals.csv exists)")
            continue
        # ponytail: regenerates the identical stream once per placement
        # (~0.6s/cell); dedup to per-load generation + copy if that ever hurts
        gen_arrivals.main(
            [
                "--rho",
                load,
                "--n",
                str(N_JOBS),
                "--seed",
                str(SEED),
                "--svc",
                svc_table,
                *flags,
                "--out",
                out,
            ]
        )
        print(f"done  {exp}/{combo}")


def launch(exps: list[str], root: str = ROOT) -> None:
    """Plan, deploy, and start all experiments' combos on the workers
    (workers.txt; empty/missing = local machine).

    Experiments are planned JOINTLY -- one runner per host executes its
    share of every experiment at most `slots` sims at a time -- and the
    combo->host plan lands in runs/<exp>/assignments.csv per experiment."""
    cells_by_exp: dict[str, list[tuple[str, str]]] = {}
    for exp in exps:
        cs = combos(exp)
        for combo, _load in cs:
            if not os.path.isfile(
                os.path.join(root, "runs", exp, combo, "arrivals.csv")
            ):
                raise SystemExit(
                    f"runs/{exp}/{combo}/arrivals.csv missing -- run gen first"
                )
        cells_by_exp[exp] = cs

    host_slots = launcher.probe_all(
        launcher.parse_workers(os.path.join(root, "workers.txt"))
    )
    if launcher.LOCAL not in host_slots and not os.path.isfile(ASTRA_SIM_BIN):
        raise SystemExit(f"binary not found: {ASTRA_SIM_BIN} (run build.sh first)")

    # combo names repeat across experiments, so plan over exp-qualified ids
    qualified = [
        (f"{exp}/{combo}", load)
        for exp, cs in cells_by_exp.items()
        for combo, load in cs
    ]
    plan = launcher.plan_assignments(qualified, host_slots)
    per_exp: dict[str, list[tuple[str, str, str]]] = {e: [] for e in exps}
    per_host: dict[str, list[tuple[str, str, str]]] = {h: [] for h in host_slots}
    for qcombo, load, host in plan:
        exp, combo = qcombo.split("/", 1)
        per_exp[exp].append((combo, load, host))
        per_host[host].append((exp, combo, load))
    for exp in exps:
        launcher.write_assignments(
            os.path.join(root, "runs", exp, "assignments.csv"), per_exp[exp]
        )

    stage = tempfile.mkdtemp(prefix="cluster4096_deploy_")

    def deploy_and_start(host: str) -> None:
        cells = per_host[host]
        cells.sort(key=lambda c: -float(c[2]))  # longest sims first (LPT)
        launcher.deploy(
            host,
            root,
            ASTRA_SIM_BIN,
            stage,
            [f"runs/{e}/{c}/arrivals.csv" for e, c, _ in cells],
        )
        launcher.start_runner(
            host, root, [(e, c) for e, c, _ in cells], host_slots[host]
        )

    with concurrent.futures.ThreadPoolExecutor(len(host_slots)) as ex:
        for _ in ex.map(deploy_and_start, [h for h in per_host if per_host[h]]):
            pass
    total = sum(len(v) for v in per_host.values())
    print(f"launched {total} combos on {len(host_slots)} host(s)")

    monitor(exps, list(host_slots), root)
    for exp in exps:
        collect(exp, root)


def progress_table(
    exp: str,
    cells: dict[str, tuple[int, str]],
    total: int,
) -> str:
    """One experiment's progress as a placement x load grid. A cell shows the
    combo's completed-job count, or ✓ once its sim finished rc=0 (completions
    can be < total when jobs were dropped), or ✗N on a nonzero exit."""
    admission = exp.split("-")[0]
    lines = [f"{exp}  (jobs done, of {total} per combo)"]
    lines.append("  " + "placement".ljust(14) + "".join(x.rjust(7) for x in LOADS))
    for pol in PLACEMENTS:
        row = "  " + pol.ljust(14)
        for load in LOADS:
            n, rc = cells.get(f"{admission}-{pol}-load{load}", (0, "-"))
            if rc == "rc=0":
                cell = "✓"
            elif rc != "-":
                cell = "✗" + rc.removeprefix("rc=")
            else:
                cell = str(n)
            row += cell.rjust(7)
        lines.append(row)
    return "\n".join(lines)


def _trace_len(exp: str, root: str) -> int:
    """Jobs per combo of an experiment = rows of its (identical) arrivals."""
    combo, _ = combos(exp)[0]
    path = os.path.join(root, "runs", exp, combo, "arrivals.csv")
    try:
        with open(path) as f:
            return max(0, sum(1 for _ in f) - 1)
    except OSError:
        return N_JOBS


def monitor(exps: list[str], hosts: list[str], root: str = ROOT) -> None:
    """Block until every launched combo has a sim.done, polling all workers
    every POLL_SECS and printing per-experiment progress tables."""
    while True:
        cells: dict[tuple[str, str], tuple[int, str]] = {}
        with concurrent.futures.ThreadPoolExecutor(max(1, len(hosts))) as ex:
            for snap in ex.map(lambda h: launcher.poll(h, root), hosts):
                cells.update(snap)
        now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"\n=== progress @ {now} ===")
        for exp in exps:
            per_exp = {c: v for (e, c), v in cells.items() if e == exp}
            print(progress_table(exp, per_exp, _trace_len(exp, root)))
        pending = sum(1 for _, rc in cells.values() if rc == "-")
        if pending == 0:
            print("all combos finished")
            return
        print(f"{pending} combos still running; next check in {POLL_SECS}s")
        time.sleep(POLL_SECS)


def collect(exp: str, root: str = ROOT) -> None:
    """Pull the experiment's results from its remote workers into the local
    combo folders (per assignments.csv) and summarize completion. Safe to run
    mid-sweep: jct.csv/occupancy.csv are streamed, so partial results land."""
    apath = os.path.join(root, "runs", exp, "assignments.csv")
    if not os.path.isfile(apath):
        raise SystemExit(f"{apath} not found -- launch first")
    rows = launcher.read_assignments(apath)
    remote_hosts = sorted({h for _, _, h in rows if h != launcher.LOCAL})
    with concurrent.futures.ThreadPoolExecutor(max(1, len(remote_hosts))) as ex:
        for _ in ex.map(lambda h: launcher.collect_host(h, root, exp), remote_hosts):
            pass

    done = failed = running = 0
    for combo, _load, _host in rows:
        try:
            rc = open(os.path.join(root, "runs", exp, combo, "sim.done")).read()
        except OSError:
            running += 1
            continue
        if rc.strip() == "rc=0":
            done += 1
        else:
            failed += 1
            print(f"  FAILED {exp}/{combo}: {rc.strip()}")
    print(
        f"collect {exp}: {done}/{len(rows)} combos done, "
        f"{failed} failed, {running} still running"
    )


def postprocess(exp: str, root: str = ROOT) -> None:
    raise SystemExit("postprocess: not implemented yet")


def clean(root: str = ROOT) -> None:
    """Remove all results: every experiment's runs/<exp> folder. The
    prerequisites (tracelib, service_times.csv) are kept -- they are
    experiment-independent and expensive to rebuild."""
    for exp in EXPERIMENTS:
        d = os.path.join(root, "runs", exp)
        if os.path.isdir(d):
            shutil.rmtree(d)
            print(f"removed {d}")
