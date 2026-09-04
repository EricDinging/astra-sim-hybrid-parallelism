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

While the sim runs it is snapshotted in place at every CKPT_MILESTONES
completed jobs (SIGUSR1 safe point + `criu dump -R`, same recipe as
ckpt.py --leave-running): the image, paused-time output copies and a
ckpt_manifest.json land in <combo>/criu-img, replacing the previous
milestone's. Snapshots are best effort -- a missing criu or a failed dump
is logged and the sim keeps running.
"""

from __future__ import annotations

import datetime
import fcntl
import hashlib
import json
import math
import os
import resource
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time

from progress import jobs_done

# Live snapshots at these completed-job counts; each replaces the previous.
CKPT_MILESTONES = tuple(range(10_000, 50_001, 10_000))
CKPT_POLL_SECS = 60
# Safe-point wait (see ckpt.STOP_TIMEOUT_S: a swap-bound rfold took >2 min).
CKPT_STOP_TIMEOUT_S = 1800
# criu command; tests point it at a stub.
CRIU = os.environ.get("CRIU", "sudo -n criu")


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


def proc_state(pid: int) -> str | None:
    """/proc state letter (R, S, T, ...) or None once the process is gone."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().rsplit(")", 1)[1].split()[0]
    except OSError:
        return None


def snapshot(pid: int, combo_dir: str, runs: str, jobs: int) -> None:
    """Dump the running sim `pid` at its next safe point into
    <combo>/criu-img, leaving it running. Built in criu-img.new and swapped
    in only on success, so a failed dump keeps the previous milestone.
    Host-wide flock: parallel dumps of ~15 GB images thrash one disk, and
    serializing bounds the transient (old + new image) to one sim."""
    with open(os.path.join(runs, ".ckpt.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        new, img = combo_dir + "/criu-img.new", combo_dir + "/criu-img"
        shutil.rmtree(new, ignore_errors=True)
        os.makedirs(new)
        os.kill(pid, signal.SIGUSR1)
        for _ in range(CKPT_STOP_TIMEOUT_S):
            st = proc_state(pid)
            if st is None:
                raise RuntimeError("sim exited before the safe point")
            if st == "T":
                break
            time.sleep(1)
        else:
            os.kill(pid, signal.SIGCONT)
            raise RuntimeError(f"not at safe point after {CKPT_STOP_TIMEOUT_S}s")
        try:
            cmd = (
                f"{CRIU} dump -t {pid} -D {shlex.quote(new)} --shell-job -R -o dump.log"
            )
            if CRIU.startswith("sudo"):  # images are root-owned otherwise
                cmd += f" && sudo -n chown -R {os.getuid()}:{os.getgid()} {shlex.quote(new)}"
            r = subprocess.run(
                ["bash", "-c", cmd], capture_output=True, text=True, timeout=1800
            )
            if r.returncode != 0:
                raise RuntimeError(f"criu dump rc={r.returncode}: {r.stderr[-300:]}")
            # Outputs must match the image: copy them while the sim is paused.
            os.makedirs(new + "/outputs")
            for f in os.listdir(combo_dir):
                p = os.path.join(combo_dir, f)
                if os.path.isfile(p) and f != "arrivals.csv":
                    shutil.copy2(p, new + "/outputs/")
        finally:
            os.kill(pid, signal.SIGCONT)
        exe = os.readlink(f"/proc/{pid}/exe")
        with open(exe, "rb") as f:
            md5 = hashlib.md5(f.read()).hexdigest()
        with open(f"/proc/{pid}/cmdline") as f:
            argv = f.read().split("\0")
        jobs_dir = next(
            (a.split("=", 1)[1] for a in argv if a.startswith("--jobs-dir=")), None
        )
        manifest = {  # same schema as ckpt.py's, so its tooling can consume it
            "leave_running": True,
            "host": socket.gethostname(),
            "combo_dir": combo_dir,
            "jobs_dir": jobs_dir,
            "binary": exe,
            "binary_md5": md5,
            "cwd": os.readlink(f"/proc/{pid}/cwd"),
            "pid": pid,
            "jobs": jobs,
            "dumped_at": datetime.datetime.now().isoformat(timespec="seconds"),
        }
        with open(new + "/ckpt_manifest.json", "w") as f:
            json.dump(manifest, f, indent=1)
        shutil.rmtree(img, ignore_errors=True)
        os.rename(new, img)


def babysit(
    proc: subprocess.Popen,
    combo_dir: str,
    runs: str,
    milestones: tuple[int, ...] = CKPT_MILESTONES,
    poll: float = CKPT_POLL_SECS,
) -> int:
    """Wait for the sim, snapshotting once per crossed milestone (a poll that
    finds several crossed takes one snapshot, at the latest). Returns rc."""
    pending = list(milestones)
    while True:
        try:
            return proc.wait(timeout=poll)
        except subprocess.TimeoutExpired:
            pass
        n = jobs_done(combo_dir)
        due = [m for m in pending if n >= m]
        if not due:
            continue
        pending = pending[len(due) :]
        try:
            snapshot(proc.pid, combo_dir, runs, due[-1])
            print(f"snapshot {combo_dir} at {due[-1]} jobs", flush=True)
        except (OSError, RuntimeError, subprocess.SubprocessError) as e:
            print(
                f"WARN snapshot {combo_dir} at {due[-1]} jobs failed: {e}", flush=True
            )


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
    # Back the sim's ~4 GB heap with transparent huge pages (jemalloc
    # madvise; kernel default 'madvise' mode honors it, verified on the
    # farm). Replicated paired A/Bs: -14% wall on 16^3 rfold, -11% on sfc,
    # from ~2x fewer dTLB misses; results byte-identical, RSS unchanged.
    # setdefault so a host-level MALLOC_CONF override wins. NOTE: measuring
    # this needs THP_enabled:1 -- some tooling sandboxes set
    # PR_SET_THP_DISABLE, which children inherit and which silently turns
    # the madvise into a no-op.
    env.setdefault("MALLOC_CONF", "thp:always,metadata_thp:always")

    cfg = os.path.join(w, "configs")
    dims = npus_per_dim(w)
    placement, topo, extra = policy_settings(pol)
    extra += failure_flags(exp, dims)
    with open(os.path.join(combo_dir, "run.err"), "w") as err:
        proc = subprocess.Popen(
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
                # The DOR route cache's 100 GiB default "safety bound" is no
                # bound at 39 sims/host: comm pairs accrete over the 60k-job
                # trace and RSS grows for days (9.4G mean / 14.9G max at 35h,
                # swap-thrashing the fleet). 4 GiB caps the cache within the
                # launcher's 8 GiB envelope; eviction is a cheap full drop.
                "--route-cache-budget-gb=4",
                *extra,
            ],
            stdout=subprocess.DEVNULL,
            stderr=err,
            env=env,
        )
        rc = babysit(proc, combo_dir, os.path.join(w, "runs"))
    with open(done, "w") as f:
        f.write(f"rc={rc}\n")
    print(f"done {exp}/{combo} rc={rc}")
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
