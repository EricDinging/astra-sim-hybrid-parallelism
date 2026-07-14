"""Sweep harness for the t3d load-sweep experiments.

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

import cluster
import gen_arrivals
import gen_matrices
import gen_traces
import launcher
import measure_svc
import shapes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # examples/t3d
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
# 0.05..0.40 step 0.05, then 0.40..1.00 step 0.02 (denser near saturation)
LOADS = [
    f"{i / 100:.2f}" for i in dict.fromkeys([*range(5, 41, 5), *range(40, 101, 2)])
]
# `ideal` is not a binary policy: it runs sfc on a fully connected mesh
# (--fullmesh + the configs/fullmesh schedules; see run_combo.py), giving the
# placement-free JCT baseline the other policies are normalized against.
PLACEMENTS = [
    "firstfit",
    "rfold",
    "sfc",
    "l1clustering",
    "topomatch",
    "random",
    "ideal",
]


def experiments(root: str = ROOT) -> dict[str, list[str]]:
    """experiment -> gen_arrivals trace-characteristic flags, derived from
    the cluster capacity (cluster.json): pareto sweeps with max job size at
    half and quarter capacity for each admission policy, plus a uniform-size
    variant at quarter. The admission policy is the experiment-name prefix
    (easy/swf/fifo); it only matters at launch time, so the three
    *-pareto<half>-* experiments generate identical (seed-pinned) arrivals.

    The fail<pct> experiments are fifo-pareto<quarter> twins with a fraction
    of NPUs marked permanently failed (run_combo.py turns the pct into the
    binary's --failure-prob, rounding the failed-node count up)."""
    cap = cluster.capacity(cluster.load(root))
    half, quarter = str(cap // 2), str(cap // 4)
    pareto = ["--alpha", "0.5", "--size-max"]
    fail = {
        f"fifo-pareto{quarter}-fail{pct}-load-sweep": [*pareto, quarter]
        for pct in ("0.1", "0.2", "0.5", "1")
    }
    return {
        f"easy-pareto{half}-load-sweep": [*pareto, half],
        f"easy-pareto{quarter}-load-sweep": [*pareto, quarter],
        f"easy-uniform{quarter}-load-sweep": [
            "--size-dist",
            "uniform",
            "--size-max",
            quarter,
        ],
        f"swf-pareto{half}-load-sweep": [*pareto, half],
        f"fifo-pareto{half}-load-sweep": [*pareto, half],
        f"ljsf-pareto{half}-load-sweep": [*pareto, half],
        f"swf-pareto{quarter}-load-sweep": [*pareto, quarter],
        f"fifo-pareto{quarter}-load-sweep": [*pareto, quarter],
        f"ljsf-pareto{quarter}-load-sweep": [*pareto, quarter],
        **fail,
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


def sync_network_yml(root: str, cap: int) -> None:
    """Keep configs/network.yml's npus_count in lockstep with cluster.json
    (the user's prereq answer is the source of truth) -- the binary reads the
    topology size from the yml, so the two must never drift."""
    p = os.path.join(root, "configs", "network.yml")
    if not os.path.isfile(p):
        raise SystemExit(f"{p} not found -- broken example checkout?")
    with open(p) as f:
        lines = f.readlines()
    for i, ln in enumerate(lines):
        if ln.startswith("npus_count:"):
            new = f"npus_count: [ {cap} ]\n"
            if ln != new:
                lines[i] = new
                with open(p, "w") as f:
                    f.writelines(lines)
                print(f"updated {p}: npus_count -> {cap}")
            return
    raise SystemExit(f"no npus_count line in {p}")


def _yml_scalar(path: str, key: str) -> float:
    """The single value of a `key: [ v ]` line in network.yml."""
    with open(path) as f:
        for ln in f:
            if ln.startswith(f"{key}:"):
                return float(ln.split("[", 1)[1].split("]")[0].strip())
    raise SystemExit(f"no {key} line in {path}")


def build_schedules(root: str, dims: tuple[int, int, int]) -> None:
    """(Re)build the BW/LT schedule matrices from the cluster dims and
    network.yml's per-link bandwidth/latency, flat in configs/ with a
    topology suffix: *_schedule_torus.txt (every policy but ideal) and
    *_schedule_fullmesh.txt (the `ideal` policy). The matrices are generated
    artifacts, never checked in. A build is skipped only when the existing
    file already matches dims, value, and topology (a fullmesh first row has
    exactly one zero -- the diagonal)."""
    cap = cluster.capacity(dims)
    yml = os.path.join(root, "configs", "network.yml")
    for topo in ("torus", "fullmesh"):
        for tag, base, key in (
            ("BW", "bandwidth", "bandwidth"),
            ("LT", "latency", "latency"),
        ):
            v = _yml_scalar(yml, key)
            value = int(v) if v == int(v) else v
            p = os.path.join(root, "configs", f"{base}_schedule_{topo}.txt")
            if os.path.isfile(p):
                with open(p) as f:
                    f.readline()  # "<tag> 0" block header
                    row = f.readline().split()
                zeros = sum(1 for x in row if float(x) == 0.0)
                if (
                    len(row) == cap
                    and {float(x) for x in row} == {0.0, float(value)}
                    and (zeros == 1) == (topo == "fullmesh")
                ):
                    continue
            gen_matrices.write_matrix(p, tag, cap, *dims, value, topo)
            print(f"built {p}: {cluster.fmt(dims)} {topo}, per-link {tag}={value}")


def prereq(root: str = ROOT, jobs: str | None = None) -> None:
    """Build the shared prerequisites for every experiment: the Chakra
    tracelib (idempotent/resumable -- already-built shapes are skipped) and
    the measured service_times.csv (skipped when present; delete it to force
    a re-measure). Neither is touched by clean(). Requires cluster.json
    (./reproduce.py prereq prompts for it)."""
    dims = cluster.load(root)
    shapes.init(dims)
    sync_network_yml(root, cluster.capacity(dims))
    build_schedules(root, dims)
    rc = gen_traces.main(
        ["--out", os.path.join(root, "tracelib"), "--cluster-dims", cluster.fmt(dims)]
        + (["--jobs", jobs] if jobs else [])
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
            "--cluster-dims",
            cluster.fmt(cluster.load(root)),
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
    dims_flag = ["--cluster-dims", cluster.fmt(cluster.load(root))]
    flags = experiments(root)[exp]
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
                *dims_flag,
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

    cap = cluster.capacity(cluster.load(root))
    ws = launcher.workspace(cap)
    host_slots = launcher.probe_all(
        launcher.parse_workers(os.path.join(root, "workers.txt")),
        launcher.mem_per_run_gb(cap),
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

    stage = tempfile.mkdtemp(prefix=f"cluster{cap}_deploy_")

    def deploy_and_start(host: str) -> None:
        cells = per_host[host]
        cells.sort(key=lambda c: -float(c[2]))  # longest sims first (LPT)
        launcher.deploy(
            host,
            root,
            ASTRA_SIM_BIN,
            stage,
            [f"runs/{e}/{c}/arrivals.csv" for e, c, _ in cells],
            ws,
        )
        launcher.start_runner(
            host, root, [(e, c) for e, c, _ in cells], host_slots[host], ws
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
    """One experiment's progress as a load x placement grid (loads down the
    rows so many fine-grained loads scroll vertically instead of overflowing
    the terminal width). A cell shows the combo's completed-job count, or ✓
    once its sim finished rc=0 (completions can be < total when jobs were
    dropped), or ✗N on a nonzero exit."""
    admission = exp.split("-")[0]
    w = max(6, *(len(p) for p in PLACEMENTS)) + 1  # col width fits name + counts
    lines = [f"{exp}  (jobs done, of {total} per combo)"]
    lines.append("  " + "load".ljust(6) + "".join(p.rjust(w) for p in PLACEMENTS))
    for load in LOADS:
        row = "  " + load.ljust(6)
        for pol in PLACEMENTS:
            n, rc = cells.get(f"{admission}-{pol}-load{load}", (0, "-"))
            if rc == "rc=0":
                cell = "✓"
            elif rc != "-":
                cell = "✗" + rc.removeprefix("rc=")
            else:
                cell = str(n)
            row += cell.rjust(w)
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
    ws = launcher.workspace(cluster.capacity(cluster.load(root)))
    while True:
        cells: dict[tuple[str, str], tuple[int, str]] = {}
        with concurrent.futures.ThreadPoolExecutor(max(1, len(hosts))) as ex:
            for snap in ex.map(lambda h: launcher.poll(h, root, ws), hosts):
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
    mid-sweep: progress.csv/occupancy.csv are streamed, so partial results land."""
    apath = os.path.join(root, "runs", exp, "assignments.csv")
    if not os.path.isfile(apath):
        raise SystemExit(f"{apath} not found -- launch first")
    rows = launcher.read_assignments(apath)
    ws = launcher.workspace(cluster.capacity(cluster.load(root)))
    remote_hosts = sorted({h for _, _, h in rows if h != launcher.LOCAL})
    with concurrent.futures.ThreadPoolExecutor(max(1, len(remote_hosts))) as ex:
        for _ in ex.map(
            lambda h: launcher.collect_host(h, root, exp, ws), remote_hosts
        ):
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
    for exp in experiments(root):
        d = os.path.join(root, "runs", exp)
        if os.path.isdir(d):
            shutil.rmtree(d)
            print(f"removed {d}")
