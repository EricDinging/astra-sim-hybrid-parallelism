"""Worker probing and combo->host packing for the launch phase.

workers.txt (one user@host per line; blank lines and #-comments ignored)
lists the servers available for simulations. An empty or missing file means
"run on the local machine". Hosts are probed at launch time for cpu count
and available memory, which bound how many sims each can run concurrently
(slots). Combos are packed highest-load-first onto the least-loaded host,
and the plan is written to runs/<exp>/assignments.csv -- the single source
of truth for launching and progress polling.
"""

from __future__ import annotations

import csv
import os
import subprocess

LOCAL = "local"
MEM_PER_RUN_GB = 24  # envelope of a high-load 4096-NPU 100k-job sim
PROBE_CMD = "nproc; awk '/MemAvailable/{print $2}' /proc/meminfo"
SSH_OPTS = [
    "-o",
    "BatchMode=yes",
    "-o",
    "ConnectTimeout=10",
    "-o",
    "StrictHostKeyChecking=accept-new",
]


def parse_workers(path: str) -> list[str]:
    """user@host lines from workers.txt; [] (=> run locally) if the file is
    missing or has no entries."""
    if not os.path.isfile(path):
        return []
    workers = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                workers.append(line)
    return workers


def probe(host: str) -> tuple[int, int]:
    """(cpus, mem_available_gb) of a worker (LOCAL or user@host)."""
    if host == LOCAL:
        cmd = ["bash", "-c", PROBE_CMD]
    else:
        cmd = ["ssh", *SSH_OPTS, host, PROBE_CMD]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if r.returncode != 0:
        raise RuntimeError(f"probe of {host} failed: {r.stderr.strip()[-200:]}")
    cpus_s, mem_kb_s = r.stdout.split()
    return int(cpus_s), int(mem_kb_s) // (1024 * 1024)


def slot_count(cpus: int, mem_gb: int) -> int:
    """Concurrent sims a host can take: cpu-bound minus headroom, and
    memory-bound at MEM_PER_RUN_GB per sim."""
    return max(0, min(cpus - 2, mem_gb // MEM_PER_RUN_GB))


def probe_all(workers: list[str]) -> dict[str, int]:
    """host -> slots for every usable worker ([] probes the local machine).
    Hosts that fail the probe or have zero slots are dropped with a warning;
    all hosts unusable is an error."""
    hosts = workers or [LOCAL]
    usable: dict[str, int] = {}
    for h in hosts:
        try:
            cpus, mem_gb = probe(h)
        except (RuntimeError, subprocess.TimeoutExpired, ValueError) as e:
            print(f"WARN dropping {h}: {e}")
            continue
        n = slot_count(cpus, mem_gb)
        if n == 0:
            print(f"WARN dropping {h}: 0 slots ({cpus} cpus, {mem_gb} GB avail)")
            continue
        usable[h] = n
        print(f"probe {h}: {cpus} cpus, {mem_gb} GB avail -> {n} slots")
    if not usable:
        raise SystemExit("no usable workers (all probes failed or 0 slots)")
    return usable


def plan_assignments(
    combos: list[tuple[str, str]],
    host_slots: dict[str, int],
) -> list[tuple[str, str, str]]:
    """Pack (combo, load) cells onto hosts as (combo, load, host) rows.

    Highest load first (longest sims), each onto the host with the smallest
    assigned-work/slots ratio, so total expected work is balanced in
    proportion to each host's parallel capacity. Work proxy = load.
    Deterministic: ties break on host order in host_slots."""
    order = list(host_slots)
    work = {h: 0.0 for h in order}
    rows = []
    for combo, load in sorted(combos, key=lambda c: (-float(c[1]), c[0])):
        h = min(order, key=lambda h: work[h] / host_slots[h])
        work[h] += float(load)
        rows.append((combo, load, h))
    rows.sort(key=lambda r: r[0])
    return rows


WORKSPACE = "/workspace/cluster4096"
BUNDLED_LIBS = ("libprotobuf", "libscotch-", "libscotcherr-", "libhwloc")


def _ssh(host: str, cmd: str, check: bool = True) -> str:
    r = subprocess.run(
        ["ssh", *SSH_OPTS, host, cmd], capture_output=True, text=True, timeout=300
    )
    if check and r.returncode != 0:
        raise RuntimeError(f"ssh {host} '{cmd[:60]}...': {r.stderr.strip()[-200:]}")
    return r.stdout


def _rsync(host: str, srcs: list[str], dst: str, *extra: str) -> None:
    cmd = ["rsync", "-az", "--partial", *extra, "-e", f"ssh {' '.join(SSH_OPTS)}"]
    cmd += [*srcs, f"{host}:{dst}"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"rsync to {host}:{dst}: {r.stderr.strip()[-200:]}")


def lib_bundle(binary: str, stage_dir: str) -> str:
    """Copy the binary's non-base shared libs (exact ABI) into
    <stage_dir>/lib and return that path; shipped as <workspace>/lib."""
    libdir = os.path.join(stage_dir, "lib")
    os.makedirs(libdir, exist_ok=True)
    out = subprocess.run(["ldd", binary], capture_output=True, text=True, check=True)
    for line in out.stdout.splitlines():
        if "=>" not in line:
            continue
        name, path = line.split("=>")[0].strip(), line.split("=>")[1].split()[0]
        if any(name.startswith(p) for p in BUNDLED_LIBS) and os.path.isfile(path):
            subprocess.run(["cp", "-u", path, libdir], check=True)
    return libdir


def deploy(
    host: str,
    root: str,
    binary: str,
    stage_dir: str,
    arrivals_files: list[str],
) -> None:
    """Provision WORKSPACE on a remote worker: binary + lib bundle + configs
    + scripts + tracelib + the assigned combos' arrivals (paths relative to
    root). Idempotent -- rsync only moves what changed; the 11 GB tracelib is
    slow the first time only. LOCAL needs no deploy (runs use root)."""
    if host == LOCAL:
        return
    w = WORKSPACE
    _ssh(host, f"mkdir -p {w}/bin {w}/lib {w}/configs {w}/scripts {w}/runs")
    _rsync(host, [binary], f"{w}/bin/")
    _rsync(host, [lib_bundle(binary, stage_dir) + "/"], f"{w}/lib/")
    _rsync(host, [os.path.join(root, "configs") + "/"], f"{w}/configs/")
    _rsync(host, [os.path.join(root, "scripts") + "/"], f"{w}/scripts/")
    _rsync(host, [os.path.join(root, "tracelib") + "/"], f"{w}/tracelib/")
    listing = os.path.join(stage_dir, f"files-{host.replace('@', '_')}.txt")
    with open(listing, "w") as f:
        f.writelines(p + "\n" for p in arrivals_files)
    _rsync(host, [root + "/"], f"{w}/", f"--files-from={listing}")


# Kill an old runner (and every sim it started under this workspace) before
# starting a new one, so a relaunch never doubles up. Twin-killing also
# happens per combo inside run_combo.sh; this sweeps combos the new plan no
# longer contains.
_KILL_OLD = (
    "pkill -f 'run_combo.py {w} ' 2>/dev/null; "
    "for pg in $(ps -eo pgid,comm,args --no-headers | "
    "awk -v w='--logging-folder={w}/' "
    "'$2==\"AstraSim_Analyt\" && index($0,w) {{print $1}}' | sort -u); do "
    'kill -9 -- "-$pg" 2>/dev/null; done; true'
)


def start_runner(
    host: str,
    root: str,
    cells: list[tuple[str, str]],
    slots: int,
) -> None:
    """Start the detached per-host runner: xargs feeds (experiment, combo)
    lines to run_combo.sh, at most `slots` concurrently. Survives SSH
    disconnect (setsid); a rerun replaces any previous runner."""
    w = root if host == LOCAL else WORKSPACE
    queue = "\n".join(f"{exp} {combo}" for exp, combo in cells) + "\n"
    runner = (
        f"cd {w} && setsid bash -c "
        f"'xargs -P {slots} -L1 python3 scripts/run_combo.py {w} < runs/queue.list' "
        f"</dev/null >runs/runner.log 2>&1 &"
    )
    if host == LOCAL:
        with open(os.path.join(w, "runs", "queue.list"), "w") as f:
            f.write(queue)
        subprocess.run(["bash", "-c", _KILL_OLD.format(w=w)], check=False)
        subprocess.run(["bash", "-c", runner], check=True)
    else:
        _ssh(host, f"cat > {w}/runs/queue.list <<'EOF'\n{queue}EOF")
        _ssh(host, _KILL_OLD.format(w=w), check=False)
        _ssh(host, runner)
    print(f"runner started on {host}: {len(cells)} combos, {slots} slots")


def poll(host: str, root: str) -> dict[tuple[str, str], tuple[int, str]]:
    """One progress snapshot of a worker's whole queue:
    (exp, combo) -> (completed_jobs, rc-string or '-')."""
    w = root if host == LOCAL else WORKSPACE
    script = os.path.join(w, "scripts", "progress.py")
    if host == LOCAL:
        r = subprocess.run(
            ["python3", script, w], capture_output=True, text=True, timeout=120
        )
        out = r.stdout if r.returncode == 0 else ""
    else:
        try:
            out = _ssh(host, f"python3 {script} {w}")
        except (RuntimeError, subprocess.TimeoutExpired) as e:
            print(f"WARN poll of {host} failed: {e}")
            return {}
    snap = {}
    for line in out.splitlines():
        exp, combo, n, rc = line.split()
        snap[(exp, combo)] = (int(n), rc)
    return snap


def collect_host(host: str, root: str, exp: str) -> None:
    """Pull a worker's results for one experiment back into the local
    runs/<exp>/ combo folders (works mid-run too: progress.csv/occupancy.csv are
    streamed). Only result files come back -- csv/sim.done/run.err;
    the verbose rotating logs (log.log*, err.log*, jct.log*, up to ~100 MB
    per combo at full trace length) stay on the worker for on-demand
    debugging, and arrivals / the shared jobs symlink dirs never move."""
    if host == LOCAL:
        return
    cmd = [
        "rsync",
        "-az",
        "--partial",
        "--exclude=jobs-load*",
        "--exclude=arrivals.csv",
        "--include=*/",
        "--include=*.csv",
        "--include=sim.done",
        "--include=run.err",
        "--include=failed_nodes.log",
        "--exclude=*",
        "--prune-empty-dirs",
        "-e",
        f"ssh {' '.join(SSH_OPTS)}",
        f"{host}:{WORKSPACE}/runs/{exp}/",
        os.path.join(root, "runs", exp) + "/",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"collect from {host}: {r.stderr.strip()[-200:]}")


def write_assignments(path: str, rows: list[tuple[str, str, str]]) -> None:
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["combo", "load", "host"])
        w.writerows(rows)


def read_assignments(path: str) -> list[tuple[str, str, str]]:
    with open(path, newline="") as f:
        return [(r["combo"], r["load"], r["host"]) for r in csv.DictReader(f)]
