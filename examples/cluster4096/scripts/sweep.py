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

import os
import shutil

import gen_arrivals
import gen_traces
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

N_JOBS = 100000
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


def launch(exp: str, root: str = ROOT) -> None:
    raise SystemExit("launch: not implemented yet")


def collect(exp: str, root: str = ROOT) -> None:
    raise SystemExit("collect: not implemented yet")


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
