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
import csv
import datetime
import io
import os
import shutil
import subprocess
import tempfile
import time
from collections.abc import Callable, Iterator

import ckpt
import cluster
import gen_arrivals
import gen_matrices
import gen_traces
import launcher
import measure_svc
import placeability
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


def build_binary(isa_floor: str) -> None:
    """(Re)build the reconfigurable binary via build.sh with the given ISA
    floor ("auto" = detect on this machine, "generic" = fully portable,
    "haswell" = require AVX2/BMI2/FMA on every machine that runs it).
    Incremental: unchanged source with the same floor rebuilds in seconds,
    and a floor change triggers the full rebuild it needs. Callers pass the
    floor probed from the machines the binary will actually run on."""
    build_sh = os.path.join(REPO, "build", "astra_analytical", "build.sh")
    print(f"building simulator binary (-a {isa_floor}) ...")
    r = subprocess.run([build_sh, "-a", isa_floor], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        raise SystemExit(f"build.sh -a {isa_floor} failed (rc={r.returncode})")
    print(f"binary ready: {ASTRA_SIM_BIN}")


# trace length; override for quick dev tests, e.g. N_JOBS=20 ./reproduce.py gen ...
# 60k (down from 65k, originally 100k): prefix analysis on the 65k sweeps
# showed stable combos converge by ~50k (<1% dev) and saturated combos scale
# linearly with trace length at any length; 60k keeps margin at the knees.
N_JOBS = int(os.environ.get("N_JOBS", 60000))
# progress-poll (and work-steal) interval while launch blocks; override for
# dev, e.g. POLL_SECS=10
POLL_SECS = int(os.environ.get("POLL_SECS", 900))
SEED = 0
# 0.02..1.00 step 0.02
LOADS = [f"{i / 100:.2f}" for i in range(2, 101, 2)]
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
    half and quarter capacity for each admission policy, plus uniform-size
    variants at quarter (easy/swf/fifo). The admission policy is the experiment-name prefix
    (easy/swf/fifo); it only matters at launch time, so the three
    *-pareto<half>-* experiments generate identical (seed-pinned) arrivals.

    The small<1..cap/16>/large<lo..hi> experiments are uniform-size traces
    confined to a capacity-relative size band (small-job-only and
    large-job-only; small1to32/large256to512 at 8x8x8, small1to256/
    large512to1024 at 16x16x16). The
    <nd>donly experiments keep the pareto<quarter> size draw but admit only
    shapes of exactly that dimensionality (gen_arrivals --ndim), drawn from
    the rfold-placeable slice of the EXPANDED shape universe (dims beyond a
    torus axis, up to the quarter cap; pp capped at 16 by the stage model;
    filtered by the idle-torus probe placeability.write_rfold_placeable so
    every remaining arm places 100% of the trace and JCT is compared on
    fully-placed workloads). They run without the firstfit placement arm,
    whose strict containment cannot place those shapes (see placements()).

    The fail<pct> experiments are easy/swf/fifo-pareto<quarter> twins with a fraction
    of NPUs marked permanently failed (run_combo.py turns the pct into the
    binary's --failure-prob, rounding the failed-node count up).

    The rounded experiments are pareto<quarter> twins whose drawn shapes are
    rounded onto a menu of very placeable targets (--snap-shapes): the
    canonical descending shapes over power-of-two dims up to the half-axis
    (= sub-block tilers of the default 4x4x4 rfold block; the packing probe
    measured fill=1.0 for all of them under firstfit AND rfold, vs 0.75-0.98
    for every 6-dim shape) plus the two-whole-block brick at the quarter cap.
    Same seed and Pareto size draws as the base trace; sizes round down except
    onto whole-block targets within 4/3x, so the mean stays ~equal (9.17 ->
    9.11 at 8x8x8). The mono experiments take this to the limit: a
    single-shape menu maps EVERY job onto the probed most placeable
    ~mean-size shape (4x2x1 at 8x8x8, 4x4x2 at 16x16x16).

    The placeability* experiments (one per failure rate, plus the no-failure
    baseline) are single-job empty-torus censuses handled entirely by
    placeability.py; their flags value is None -- they have no arrivals,
    loads, or combos, and launch runs them locally."""
    dims = cluster.load(root)
    cap = cluster.capacity(dims)
    half, quarter = str(cap // 2), str(cap // 4)
    pareto = ["--alpha", "0.5", "--size-max"]
    # snap menu: descending shapes over power-of-two dims <= half-axis
    # (sub-block tilers), plus the two-block brick (e.g. 8x4x4 = quarter)
    half_axis = max(dims) // 2
    dom = [2**k for k in range(half_axis.bit_length())]
    menu = [f"{a}x{b}x{c}" for a in dom for b in dom for c in dom if a >= b >= c] + [
        f"{2 * half_axis}x{half_axis}x{half_axis}"
    ]
    rounded = [*pareto, quarter, "--snap-shapes", ",".join(menu)]
    # mono: the rounded idea taken to the limit -- every job becomes a single
    # very placeable ~mean-size shape, probed per torus (packed-torus probe:
    # cap/|S| identical jobs at t=0, fifo, all placements).
    # - 8x8x8: 4x2x1. All six placements pack it 64/64 (tie with 8x1x1/2x2x2)
    #   and it has the best packed-torus JCT on 5 of 6; size 8 ~ mean 9.1.
    # - 16x16x16: 4x4x2 (2026-08-24). Mean pareto1024 size is 40.7; every
    #   size-32 candidate packs 128/128 under all six, so JCT decides:
    #   8x2x2 is best on 4/6 but worst under rfold (832 vs 254 ms for
    #   8x4x1), 8x4x1 is rfold's best, 4x4x2 (379 ms rfold, 2nd) is the
    #   4x4x4-block-aligned pick that no policy is badly handicapped by.
    mono_shape = {8: "4x2x1", 16: "4x4x2"}[max(dims)]
    mono = [*pareto, quarter, "--snap-shapes", mono_shape]
    fail = {
        f"{adm}-pareto{quarter}-fail{pct}-load-sweep": [*pareto, quarter]
        for pct in ("0.1", "0.2", "0.5", "1")
        for adm in ("easy", "swf", "fifo")
    }
    uniform = ["--size-dist", "uniform", "--size-max", quarter]
    small_max = str(cap // 16)
    small = ["--size-dist", "uniform", "--size-min", "1", "--size-max", small_max]
    # large band per torus: half..cap at 8x8x8 (256-512); cap/8..cap/4 at
    # 16x16x16 (512-1024) -- 2048-4096 jobs are too costly to sweep
    large_min, large_max = {8: (half, str(cap)), 16: (str(cap // 8), quarter)}[
        max(dims)
    ]
    large = ["--size-dist", "uniform", "--size-min", large_min, "--size-max", large_max]
    ndonly = {
        nd: [
            *pareto,
            quarter,
            "--ndim",
            str(nd),
            "--shapes-file",
            os.path.join(root, f"rfold_placeable{quarter}.txt"),
        ]
        for nd in (1, 2, 3)
    }
    return {
        f"easy-pareto{half}-load-sweep": [*pareto, half],
        f"swf-pareto{half}-load-sweep": [*pareto, half],
        f"fifo-pareto{half}-load-sweep": [*pareto, half],
        f"ljsf-pareto{half}-load-sweep": [*pareto, half],
        f"easy-pareto{quarter}-load-sweep": [*pareto, quarter],
        f"swf-pareto{quarter}-load-sweep": [*pareto, quarter],
        f"fifo-pareto{quarter}-load-sweep": [*pareto, quarter],
        f"ljsf-pareto{quarter}-load-sweep": [*pareto, quarter],
        f"easy-pareto{quarter}-rounded-load-sweep": rounded,
        f"swf-pareto{quarter}-rounded-load-sweep": rounded,
        f"fifo-pareto{quarter}-rounded-load-sweep": rounded,
        f"ljsf-pareto{quarter}-rounded-load-sweep": rounded,
        f"easy-pareto{quarter}-mono-load-sweep": mono,
        f"swf-pareto{quarter}-mono-load-sweep": mono,
        f"fifo-pareto{quarter}-mono-load-sweep": mono,
        f"ljsf-pareto{quarter}-mono-load-sweep": mono,
        f"easy-uniform{quarter}-load-sweep": uniform,
        f"swf-uniform{quarter}-load-sweep": uniform,
        f"fifo-uniform{quarter}-load-sweep": uniform,
        # small/large-job-only traces: sizes uniform over the distinct legal
        # sizes in the range, shape uniform among legal shapes of that size
        f"easy-small1to{small_max}-load-sweep": small,
        f"swf-small1to{small_max}-load-sweep": small,
        f"fifo-small1to{small_max}-load-sweep": small,
        f"easy-large{large_min}to{large_max}-load-sweep": large,
        f"swf-large{large_min}to{large_max}-load-sweep": large,
        f"fifo-large{large_min}to{large_max}-load-sweep": large,
        # dimensionality-restricted traces: same truncated-Pareto size draw
        # as the pareto<quarter> family, but over the sizes that have a
        # 1D/2D/3D shape, then shape uniform within that dimensionality
        **{
            f"{adm}-{nd}donly-load-sweep": ndonly[nd]
            for nd in (1, 2, 3)
            for adm in ("easy", "swf", "fifo")
        },
        # reconfigurability sweeps: same pareto<quarter> arrivals, but the
        # placements are rfold at every block granularity (see placements())
        f"easy-pareto{quarter}-blocksize-load-sweep": [*pareto, quarter],
        f"swf-pareto{quarter}-blocksize-load-sweep": [*pareto, quarter],
        f"fifo-pareto{quarter}-blocksize-load-sweep": [*pareto, quarter],
        **fail,
        **{e: None for e in placeability.EXPS},
    }


def placements(exp: str, root: str = ROOT) -> list[str]:
    """The placement columns of an experiment. The blocksize experiment sweeps
    rfold's block granularity -- rfoldb<N> = rfold with --block-size NxNxN
    (see run_combo.policy_settings), doubling from 1x1x1 up to the whole
    torus (folding-only) -- against the firstfit and ideal anchors. The
    <nd>donly experiments drop firstfit: their expanded shape universe
    contains shapes strict containment cannot place."""
    if "donly-" in exp:
        return [p for p in PLACEMENTS if p != "firstfit"]
    if "-blocksize-" not in exp:
        return PLACEMENTS
    dims = cluster.load(root)
    blocks, b = [], 1
    while all(d % b == 0 for d in dims):
        blocks.append(f"rfoldb{b}")
        b *= 2
    return ["firstfit", *blocks, "ideal"]


def combos(exp: str, root: str = ROOT) -> list[tuple[str, str]]:
    """All (combo_name, load) cells of an experiment, e.g.
    ("easy-firstfit-load0.15", "0.15"). Admission comes from the exp name."""
    admission = exp.split("-")[0]
    return [
        (f"{admission}-{placement}-load{load}", load)
        for placement in placements(exp, root)
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


def build_binaries(host_floors: dict[str, str], stage: str) -> dict[str, str]:
    """Build one binary per ISA floor among host_floors (plus the local
    machine's own, recorded as host_floors[LOCAL]) into <stage>/<floor>/
    and return floor -> staged binary path. Binaries are compiled here but
    run on the workers, so each host's own floor picks its -a flag --
    haswell hosts get the haswell build, the rest the generic one. The
    local floor is built last, leaving the in-tree binary locally
    runnable (the local machine always runs something: placeability
    probes, or the whole job when workers.txt is empty)."""
    local_floor = "haswell" if launcher.probe(launcher.LOCAL)[2] else "generic"
    host_floors[launcher.LOCAL] = local_floor
    binaries: dict[str, str] = {}
    for floor in sorted(set(host_floors.values()) - {local_floor}) + [local_floor]:
        build_binary(floor)
        os.makedirs(os.path.join(stage, floor))
        binaries[floor] = shutil.copy2(ASTRA_SIM_BIN, os.path.join(stage, floor))
    return binaries


def farm(
    root: str, workers_file: str | None
) -> Callable[[str, list[str], list[str], int, float], Iterator]:
    """Probe the workers (workers_file, default workers.txt; empty/missing
    = the local machine), build their binaries, and return run(script,
    args, items, chunk, timeout): deploy the workspace to every worker
    (idempotent rsync, so a second call after gen_traces ships only the
    new traces) and stream `items` through launcher.pull_run, invoking
    scripts/<script> --only <batch> --out /dev/stdout on each host with
    one sim per slot. Yields pull_run's (host, batch, csv_text|None, err)."""
    dims = cluster.load(root)
    cap = cluster.capacity(dims)
    ws = launcher.workspace(cap)
    host_slots, host_floors = launcher.probe_all(
        launcher.parse_workers(workers_file or os.path.join(root, "workers.txt")),
        launcher.mem_per_run_gb(cap),
    )
    stage = tempfile.mkdtemp(prefix=f"cluster{cap}_prereq_")
    binaries = build_binaries(host_floors, stage)
    remote_bin = f"{ws}/bin/{os.path.basename(ASTRA_SIM_BIN)}"

    def cmd_for(script: str, args: list[str], host: str, batch: list[str]) -> str:
        if host == launcher.LOCAL:
            w, py, binary = root, f"python3 {HERE}/{script}", ASTRA_SIM_BIN
        else:
            w = ws
            py = f"LD_LIBRARY_PATH={ws}/lib python3 {ws}/scripts/{script}"
            binary = remote_bin
        return (
            f"cd {w} && {py} --cfg configs --astra-sim {binary} "
            f"--cluster-dims {cluster.fmt(dims)} --jobs 1 --out /dev/stdout "
            f"--only {','.join(batch)} {' '.join(args)}"
        )

    def run(
        script: str, args: list[str], items: list[str], chunk: int, timeout: float
    ) -> Iterator[tuple[str, list[str], str | None, str]]:
        with concurrent.futures.ThreadPoolExecutor(len(host_slots)) as ex:
            for _ in ex.map(
                lambda h: launcher.deploy(
                    h, root, binaries[host_floors[h]], stage, [], ws
                ),
                host_slots,
            ):
                pass
        print(
            f"{script}: {len(items)} items on {len(host_slots)} host(s), "
            f"{sum(host_slots.values())} slots"
        )
        return launcher.pull_run(
            host_slots, items, chunk, lambda h, b: cmd_for(script, args, h, b), timeout
        )

    return run


PROBE_CHUNK = 50


def prereq(
    root: str = ROOT, jobs: str | None = None, workers_file: str | None = None
) -> None:
    """Build the shared prerequisites for every experiment: the Chakra
    tracelib (idempotent/resumable -- already-built shapes are skipped) and
    the measured service_times.csv (shapes already in it are skipped;
    delete it to force a full re-measure). Neither is touched by clean().
    Requires cluster.json (./reproduce.py prereq prompts for it).

    The placeability probe and the service-time measure run on the
    workers in workers_file (see farm; empty/missing = local machine),
    slot-limited per host with dynamic balancing."""
    dims = cluster.load(root)
    shapes.init(dims)
    sync_network_yml(root, cluster.capacity(dims))
    build_schedules(root, dims)
    run = farm(root, workers_file)
    # the <nd>donly experiments sample the rfold-placeable slice of the
    # expanded (non-torus-fitting) universe at the quarter cap: probe it
    # with the actual binary FIRST (no traces needed), then build/measure
    # only the union of the standard universe and that placeable slice
    cap = cluster.capacity(dims) // 4
    placeable = placeability.rfold_placeable_path(root, cap)
    if os.path.isfile(placeable):
        print(f"skip  {placeable} (exists; delete it to re-probe)")
    else:
        cands = shapes.expanded_legal_shapes("bw", cap)
        outcomes: dict[str, str] = {}
        for host, batch, out, err in run(
            "placeability.py",
            [],
            [shapes.fmt_shape(s) for s in cands],
            PROBE_CHUNK,
            placeability.PROBE_TIMEOUT_S * PROBE_CHUNK + 120,
        ):
            if out is None:
                print(f"WARN {host}: probe batch of {len(batch)} failed: {err}")
                continue
            for row in csv.DictReader(io.StringIO(out)):
                outcomes[row["shape"]] = row["outcome"]
            print(f"  probed {len(outcomes)}/{len(cands)}")
        placeability.write_rfold_placeable(placeable, cands, outcomes)
    rc = gen_traces.main(
        [
            "--out",
            os.path.join(root, "tracelib"),
            "--cluster-dims",
            cluster.fmt(dims),
            "--extra-shapes-file",
            placeable,
        ]
        + (["--jobs", jobs] if jobs else [])
    )
    if rc:
        raise SystemExit(rc)
    svc(root, run, placeable)


def svc(root: str, run: Callable, extra_file: str | None = None) -> None:
    """Measure the isolated service time of every shape with a trace that
    is not yet in service_times.csv (standard universe + extra_file's
    shapes), through `run` (see farm), largest first. The table is
    rewritten after every result, so an interrupted measure resumes where
    it stopped; a partially failed one (e.g. OOM on the big shapes) is
    re-tried on the next call. Delete the table to re-measure everything."""
    table = os.path.join(root, "service_times.csv")
    have = measure_svc.read_table(table)
    todo = [
        s
        for s in measure_svc.legal_shapes_with_traces(
            "bw", os.path.join(root, "tracelib"), None, extra_file
        )
        if s not in have
    ]
    if not todo:
        print(f"skip  {table} ({len(have)} shapes; delete it to re-measure)")
        return
    todo.sort(key=lambda s: (-s[0] * s[1] * s[2], s))
    print(f"measuring {len(todo)} shapes ({len(have)} already in {table})")
    rows = [(s, s[0] * s[1] * s[2], v) for s, v in have.items()]
    failures = []
    for host, batch, out, err in run(
        "measure_svc.py",
        ["--traces", "tracelib"],
        [shapes.fmt_shape(s) for s in todo],
        1,
        measure_svc.SIM_TIMEOUT_S + 300,
    ):
        if out is None:
            failures.append(f"{','.join(batch)} on {host}: {err}")
            continue
        for r in csv.DictReader(io.StringIO(out)):
            rows.append(
                (
                    shapes.parse_shape(r["shape"]),
                    int(r["size"]),
                    int(r["svc_per_iter_ns"]),
                )
            )
        measure_svc.write_table(table, rows)
        print(f"  {len(rows) - len(have)}/{len(todo)} measured ({batch[0]} on {host})")
    print(f"wrote {table}: {len(rows)} shapes, {len(failures)} failed")
    for f in failures:
        print(f"  FAIL {f}")
    if failures:
        raise SystemExit(1)


def gen(exp: str, root: str = ROOT) -> None:
    """Generate arrival traces for every combo of the experiment, directly
    into runs/<exp>/<combo>/ (combos with an arrivals.csv are skipped, so
    re-running is cheap and resumable). All placements at one load get the
    same seed-pinned job stream, for a fair policy comparison."""
    if experiments(root)[exp] is None:  # placeability: nothing to generate
        print(f"skip  {exp} (placeability census needs no arrivals)")
        return
    svc_table = os.path.join(root, "service_times.csv")
    if not os.path.isfile(svc_table):
        raise SystemExit(
            f"{svc_table} not found -- generate it with ./reproduce.py prereq"
        )
    dims_flag = ["--cluster-dims", cluster.fmt(cluster.load(root))]
    flags = experiments(root)[exp]
    for combo, load in combos(exp, root):
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


def launch(exps: list[str], root: str = ROOT, workers_file: str | None = None) -> None:
    """Plan, deploy, and start all experiments' combos on the workers
    (workers_file, default workers.txt; empty/missing = local machine).
    Different launches may use different worker files (separate groups);
    each experiment's assignments.csv records its hosts, so monitor and
    collect later target exactly the group a sweep runs on.

    Experiments are planned JOINTLY -- one runner per host executes its
    share of every experiment at most `slots` sims at a time -- and the
    combo->host plan lands in runs/<exp>/assignments.csv per experiment.
    The plan is only the starting point: the monitor loop this blocks in
    afterwards steals queued combos onto hosts that run dry (see monitor).

    Placeability experiments have no combos to distribute; each is a local
    ~0.1s-per-probe census, run to completion before the real launch."""
    cells_by_exp: dict[str, list[tuple[str, str]]] = {}
    for exp in [e for e in exps if experiments(root)[e] is not None]:
        cs = combos(exp, root)
        for combo, _load in cs:
            if not os.path.isfile(
                os.path.join(root, "runs", exp, combo, "arrivals.csv")
            ):
                raise SystemExit(
                    f"runs/{exp}/{combo}/arrivals.csv missing -- run gen first"
                )
        cells_by_exp[exp] = cs

    # Probe the workers first: each host's ISA floor picks its binary
    cap = cluster.capacity(cluster.load(root))
    ws = launcher.workspace(cap)
    host_slots, host_floors = launcher.probe_all(
        launcher.parse_workers(workers_file or os.path.join(root, "workers.txt")),
        launcher.mem_per_run_gb(cap, exps),
    )
    stage = tempfile.mkdtemp(prefix=f"cluster{cap}_deploy_")
    binaries = build_binaries(host_floors, stage)

    for exp in [e for e in exps if experiments(root)[e] is None]:
        placeability.run(exp, root, ASTRA_SIM_BIN)
    exps = [e for e in exps if experiments(root)[e] is not None]
    if not exps:
        return

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

    def deploy_and_start(host: str) -> None:
        cells = per_host[host]  # queue-entry names give LPT order, no sort
        if host != launcher.LOCAL:
            ckpt.ensure_criu(host)  # run_combo's milestone snapshots need it
        launcher.deploy(
            host,
            root,
            binaries[host_floors[host]],
            stage,
            [f"runs/{e}/{c}/arrivals.csv" for e, c, _ in cells],
            ws,
        )
        launcher.start_runner(host, root, cells, host_slots[host], ws)

    with concurrent.futures.ThreadPoolExecutor(len(host_slots)) as ex:
        for _ in ex.map(deploy_and_start, [h for h in per_host if per_host[h]]):
            pass
    total = sum(len(v) for v in per_host.values())
    print(f"launched {total} combos on {len(host_slots)} host(s)")

    monitor(exps, root)
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
    dropped), or ✗N on a nonzero exit. Not-finished states (q = queued,
    run = claimed by a runner slot, - = legacy/unknown) all show the
    count."""
    admission = exp.split("-")[0]
    pols = placements(exp)
    w = max(6, *(len(p) for p in pols)) + 1  # col width fits name + counts
    lines = [f"{exp}  (jobs done, of {total} per combo)"]
    lines.append("  " + "load".ljust(6) + "".join(p.rjust(w) for p in pols))
    for load in LOADS:
        row = "  " + load.ljust(6)
        for pol in pols:
            n, rc = cells.get(f"{admission}-{pol}-load{load}", (0, "-"))
            if rc == "rc=0":
                cell = "✓"
            elif rc.startswith("rc="):
                cell = "✗" + rc.removeprefix("rc=")
            else:  # "q" / "run" / "-": not finished yet
                cell = str(n)
            row += cell.rjust(w)
        lines.append(row)
    return "\n".join(lines)


def _trace_len(exp: str, root: str) -> int:
    """Jobs per combo of an experiment = rows of its (identical) arrivals."""
    combo, _ = combos(exp, root)[0]
    path = os.path.join(root, "runs", exp, combo, "arrivals.csv")
    try:
        with open(path) as f:
            return max(0, sum(1 for _ in f) - 1)
    except OSError:
        return N_JOBS


def reassign(exp: str, combo: str, host: str, root: str = ROOT) -> None:
    """Point a stolen combo's assignments.csv row at its new host, so
    collect pulls its results from where the combo actually runs."""
    path = os.path.join(root, "runs", exp, "assignments.csv")
    rows = [
        (c, load, host if c == combo else h)
        for c, load, h in launcher.read_assignments(path)
    ]
    launcher.write_assignments(path, rows)


def monitor(exps: list[str], root: str = ROOT) -> None:
    """Block until every launched combo has a sim.done, polling every
    POLL_SECS and printing per-experiment progress tables. Each experiment's
    workers come from its runs/<exp>/assignments.csv, so sweeps launched on
    different worker groups are each polled on their own group (and
    monitoring all polls the union). Unlaunched experiments are skipped.

    Between polls the queues are rebalanced: hosts that ran dry steal
    queued-but-unstarted combos from backlogged hosts (sim durations vary
    too much for the static launch plan to stay balanced), and
    assignments.csv follows the moves."""
    exps = [e for e in exps if experiments(root)[e] is not None]
    launched = []
    for exp in exps:
        if os.path.isfile(os.path.join(root, "runs", exp, "assignments.csv")):
            launched.append(exp)
        else:
            print(f"skip  {exp} (no assignments.csv -- not launched)")
    exps = launched
    if not exps:
        return
    hosts = sorted(
        {
            h
            for exp in exps
            for _, _, h in launcher.read_assignments(
                os.path.join(root, "runs", exp, "assignments.csv")
            )
        }
    )
    cap = cluster.capacity(cluster.load(root))
    ws = launcher.workspace(cap)
    host_slots = launcher.probe_slots(hosts, launcher.mem_per_run_gb(cap, exps))
    while True:
        with concurrent.futures.ThreadPoolExecutor(max(1, len(hosts))) as ex:
            snaps = dict(
                zip(hosts, ex.map(lambda h: launcher.poll(h, root, ws), hosts))
            )
        for exp, combo, src, dst in launcher.rebalance(
            snaps, host_slots, exps, root, ws
        ):
            reassign(exp, combo, dst, root)
            print(f"steal {exp}/{combo}: {src} -> {dst}")
        cells: dict[tuple[str, str], tuple[int, str]] = {}
        for snap in snaps.values():
            cells.update(snap)
        # a host's queue may also carry other sweeps' combos; count only ours
        cells = {k: v for k, v in cells.items() if k[0] in exps}
        now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"\n=== progress @ {now} ===")
        for exp in exps:
            per_exp = {c: v for (e, c), v in cells.items() if e == exp}
            print(progress_table(exp, per_exp, _trace_len(exp, root)))
        pending = sum(1 for _, rc in cells.values() if not rc.startswith("rc="))
        if pending == 0:
            launcher.stop_runners(hosts, root, ws)
            print("all combos finished")
            return
        print(f"{pending} combos still running; next check in {POLL_SECS}s")
        time.sleep(POLL_SECS)


def collect(exp: str, root: str = ROOT) -> None:
    """Pull the experiment's results from its remote workers into the local
    combo folders (per assignments.csv) and summarize completion. Safe to run
    mid-sweep: progress.csv/occupancy.csv are streamed, so partial results land."""
    if experiments(root)[exp] is None:  # placeability ran locally; just report
        placeability.summarize(exp, root)
        return
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
