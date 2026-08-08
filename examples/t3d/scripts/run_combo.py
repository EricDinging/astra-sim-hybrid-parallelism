#!/usr/bin/env python3
"""Run one combo's simulation: run_combo.py <workspace> <experiment> <combo>.

All outputs (progress.csv, occupancy.csv, jobs.csv, summary.txt, logs) land in the
combo folder <workspace>/runs/<experiment>/<combo>/. Invoked by the per-host
runner (xargs -P <slots>) on a remote worker (workspace =
/workspace/cluster<capacity>) or locally (workspace = the example dir).

Idempotent: skips a combo whose sim.done says rc=0, and kills a stale twin of
itself (a sim logging to the same combo folder) before starting, so a
relaunch never stacks two sims on one combo. Dependency-free stdlib so it
runs unchanged on any worker.
"""

from __future__ import annotations

import fcntl
import json
import math
import os
import resource
import subprocess
import sys


def npus_per_dim(w: str) -> str:
    """The torus dims from the workspace's cluster.json (deployed alongside
    configs/), as the binary's --npus-per-dim value."""
    p = os.path.join(w, "cluster.json")
    try:
        with open(p) as f:
            return ",".join(str(d) for d in json.load(f)["dims"])
    except (OSError, KeyError, ValueError) as e:
        raise SystemExit(f"{p} missing or invalid ({e}) -- redeploy/run prereq")


def find_binary(w: str) -> str:
    for p in (
        os.path.join(w, "bin", "AstraSim_Analytical_Reconfigurable"),
        # local mode: workspace is examples/t3d, binary in the repo tree
        os.path.join(
            w,
            "..",
            "..",
            "build",
            "astra_analytical",
            "build",
            "bin",
            "AstraSim_Analytical_Reconfigurable",
        ),
    ):
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return os.path.abspath(p)
    raise SystemExit(f"no binary under {w}/bin or the local build tree")


# comm(16) truncation of AstraSim_Analytical_Reconfigurable; also imported by
# ckpt.py, so the fleet tools and this runner agree on how to find a sim.
BIN_COMM = "AstraSim_Analyt"


def live_sim_pgids(combo_dir: str) -> set[int]:
    """Process groups of sims logging to this combo folder. Match on comm,
    not args -- matching args would find this script's own command line."""
    ps = subprocess.run(
        ["ps", "-eo", "pgid,comm,args", "--no-headers"],
        capture_output=True,
        text=True,
    ).stdout
    needle = f"--logging-folder={combo_dir} "
    pgids = set()
    for line in ps.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] == BIN_COMM and needle in parts[2]:
            pgids.add(int(parts[0]))
    return pgids


def kill_stale_twin(combo_dir: str) -> None:
    """Kill any sim already logging to this combo folder (whole group)."""
    for pg in live_sim_pgids(combo_dir):
        try:
            os.killpg(pg, 9)
        except (ProcessLookupError, PermissionError):
            pass


def build_jobs_dir(w: str, exp: str, combo_dir: str, load: str) -> str:
    """jobs/ symlinks are shared by every placement at one load (identical
    seed-pinned arrivals); an flock serializes the concurrent first-builders."""
    jobs = os.path.join(w, "runs", exp, f"jobs-load{load}")
    lock_path = os.path.join(w, "runs", exp, f".jobs-load{load}.lock")
    done_marker = os.path.join(jobs, ".complete")
    with open(lock_path, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if os.path.exists(done_marker):
            # Already fully built: re-running make_jobs would unlink+relink
            # every entry, racing sims already reading this shared dir.
            return jobs
        subprocess.run(
            [
                sys.executable,
                os.path.join(w, "scripts", "make_jobs.py"),
                "--arrivals",
                os.path.join(combo_dir, "arrivals.csv"),
                "--tracelib",
                os.path.join(w, "tracelib"),
                "--out",
                jobs,
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        with open(done_marker, "w"):
            pass
    return jobs


def failure_flags(exp: str, dims_csv: str) -> list[str]:
    """--failure-prob for a fail<pct> experiment (e.g. fifo-pareto128-fail0.5-
    load-sweep -> 0.5% of the NPUs failed). The binary picks lround(p*N) nodes
    (fixed seed 42), so pass ceil(pct/100*N)/N to round the count *up*."""
    for tok in exp.split("-"):
        if tok.startswith("fail"):
            cap = math.prod(int(d) for d in dims_csv.split(","))
            k = math.ceil(float(tok[len("fail") :]) / 100 * cap)
            return [f"--failure-prob={k / cap}"]
    return []


def policy_settings(pol: str) -> tuple[str, str, list[str]]:
    """(placement-policy, schedule-file topology suffix, extra flags) for a
    combo's placement. `ideal` is sfc on a fully connected mesh: same arrivals
    and jobs, but the *_schedule_fullmesh.txt matrices and the binary's
    --fullmesh direct routing. `rfoldb<N>` is rfold with an NxNxN
    --block-size (the blocksize experiment's reconfigurability knob)."""
    if pol == "ideal":
        return "sfc", "fullmesh", ["--fullmesh"]
    if pol.startswith("rfoldb"):
        b = pol.removeprefix("rfoldb")
        return "rfold", "torus", [f"--block-size={b}x{b}x{b}"]
    return pol, "torus", []


def main() -> int:
    w, exp, combo = os.path.abspath(sys.argv[1]), sys.argv[2], sys.argv[3]
    combo_dir = os.path.join(w, "runs", exp, combo)
    # combo = <admission>-<placement>-load<L>
    adm, pol, load_part = combo.split("-", 2)
    load = load_part.removeprefix("load")

    done = os.path.join(combo_dir, "sim.done")
    arrivals = os.path.join(combo_dir, "arrivals.csv")
    if os.path.isfile(done) and open(done).read().strip() == "rc=0":
        # regenerated arrivals (rsync preserves mtimes) invalidate old results
        if os.path.getmtime(arrivals) < os.path.getmtime(done):
            print(f"skip {exp}/{combo} (done)")
            return 0
        print(f"rerun {exp}/{combo} (arrivals newer than sim.done)")

    # A sim restored by ckpt.py resume runs unwrapped (no run_combo parent)
    # and has no sim.done yet; killing it as a "stale twin" would restart the
    # combo from tick 0 and throw away the checkpointed progress.
    if os.path.isfile(os.path.join(combo_dir, "criu-img", "restored.at")):
        if live_sim_pgids(combo_dir):
            print(f"skip {exp}/{combo} (ckpt.py-restored sim in flight)")
            return 0

    kill_stale_twin(combo_dir)
    if os.path.isfile(done):
        os.remove(done)

    try:
        jobs = build_jobs_dir(w, exp, combo_dir, load)
    except subprocess.CalledProcessError:
        with open(done, "w") as f:
            f.write("rc=98\n")
        print(f"FAIL {exp}/{combo}: make_jobs")
        return 1

    # one open .et fd per occupied NPU; the biggest shape needs the full torus
    _soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = (
        os.path.join(w, "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
    )

    cfg = os.path.join(w, "configs")
    dims = npus_per_dim(w)
    placement, topo, extra = policy_settings(pol)
    extra += failure_flags(exp, dims)
    with open(os.path.join(combo_dir, "run.err"), "w") as err:
        rc = subprocess.run(
            [
                find_binary(w),
                f"--system-configuration={cfg}/system.json",
                f"--remote-memory-configuration={cfg}/remote_memory.json",
                f"--network-configuration={cfg}/network.yml",
                f"--bw-schedule={cfg}/bandwidth_schedule_{topo}.txt",
                f"--latency-schedule={cfg}/latency_schedule_{topo}.txt",
                f"--logging-folder={combo_dir}",
                "--num-queues-per-dim=1",
                "--comm-scale=1.0",
                "--injection-scale=1.0",
                "--rendezvous-protocol=false",
                f"--npus-per-dim={dims}",
                f"--job-arrival-file={combo_dir}/arrivals.csv",
                f"--jobs-dir={jobs}",
                f"--placement-policy={placement}",
                f"--admission-policy={adm}",
                *extra,
            ],
            stdout=subprocess.DEVNULL,
            stderr=err,
            env=env,
        ).returncode
    with open(done, "w") as f:
        f.write(f"rc={rc}\n")
    print(f"done {exp}/{combo} rc={rc}")
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
