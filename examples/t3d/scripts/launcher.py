"""Worker probing, combo->host packing, and work stealing.

workers.txt (one user@host per line; blank lines and #-comments ignored)
lists the servers available for simulations. An empty or missing file means
"run on the local machine". Hosts are probed at launch time for cpu count
and available memory, which bound how many sims each can run concurrently
(slots). Combos are packed highest-load-first onto the least-loaded host,
and the plan is written to runs/<exp>/assignments.csv -- the single source
of truth for launching and progress polling.

The static plan is only a starting point: sim durations vary wildly, so
each host's share lives in a stealable queue (runs/queue/, one prio-named
file per combo; runner.py claims entries by atomic rename). Between
monitor polls, rebalance() moves queued-but-unstarted combos from
backlogged hosts to hosts that ran dry, and monitor updates
assignments.csv so collect pulls each combo from where it actually ran.
"""

from __future__ import annotations

import concurrent.futures
import csv
import os
import subprocess

import cluster

LOCAL = "local"
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


# Target fill fraction of a host's available memory.
MEM_TARGET_PCT = 85


def mem_per_run_gb(capacity: int) -> int:
    """Per-sim memory envelope for slot packing. ponytail: 3 GB flat at
    8x8x8 -- an average over the fleet-measured mix (stable cells <=1 GB,
    supercritical high-load scatter cells ~10 GB); rs630-class hosts go
    cpu-bound before this average overfills them. Other capacities keep the
    legacy envelope (24 GB at 4096 NPUs, linear, 4 GB floor)."""
    if capacity == 512:
        return 3
    return max(4, 24 * capacity // 4096)


def workspace(capacity: int) -> str:
    """Remote workspace dir; capacity-keyed so clusters never share state."""
    return f"/workspace/cluster{capacity}"


def slot_count(cpus: int, mem_gb: int, mem_per_run: int) -> int:
    """Concurrent sims a host can take: cpu-bound minus headroom, and
    memory-bound at mem_per_run GB per sim against MEM_TARGET_PCT of the
    available memory."""
    return max(0, min(cpus - 2, mem_gb * MEM_TARGET_PCT // 100 // mem_per_run))


def probe_all(workers: list[str], mem_per_run: int) -> dict[str, int]:
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
        n = slot_count(cpus, mem_gb, mem_per_run)
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
    ws: str,
) -> None:
    """Provision the workspace `ws` on a remote worker: binary + lib bundle
    + cluster.json + configs + scripts + tracelib + the assigned combos'
    arrivals (paths relative to root). Idempotent -- rsync only moves what
    changed; the 11 GB tracelib is slow the first time only. LOCAL needs no
    deploy (runs use root)."""
    if host == LOCAL:
        return
    w = ws
    _ssh(host, f"mkdir -p {w}/bin {w}/lib {w}/configs {w}/scripts {w}/runs")
    _rsync(host, [binary], f"{w}/bin/")
    _rsync(host, [lib_bundle(binary, stage_dir) + "/"], f"{w}/lib/")
    _rsync(host, [cluster.path(root)], f"{w}/")
    _rsync(host, [os.path.join(root, "configs") + "/"], f"{w}/configs/")
    _rsync(host, [os.path.join(root, "scripts") + "/"], f"{w}/scripts/")
    _rsync(host, [os.path.join(root, "tracelib") + "/"], f"{w}/tracelib/")
    listing = os.path.join(stage_dir, f"files-{host.replace('@', '_')}.txt")
    with open(listing, "w") as f:
        f.writelines(p + "\n" for p in arrivals_files)
    _rsync(host, [root + "/"], f"{w}/", f"--files-from={listing}")


# Kill an old runner (and every sim it started under this workspace) before
# starting a new one, so a relaunch never doubles up. Twin-killing also
# happens per combo inside run_combo.py; this sweeps combos the new plan no
# longer contains. The [y] character class stops pkill -f from matching the
# remote shell that carries this very command line in its args.
_KILL_OLD = (
    "pkill -f 'runner.p[y] {w} ' 2>/dev/null; "
    "pkill -f 'run_combo.p[y] {w} ' 2>/dev/null; "
    "for pg in $(ps -eo pgid,comm,args --no-headers | "
    "awk -v w='--logging-folder={w}/' "
    "'$2==\"AstraSim_Analyt\" && index($0,w) {{print $1}}' | sort -u); do "
    'kill -9 -- "-$pg" 2>/dev/null; done; true'
)


def qname(exp: str, combo: str, load: str) -> str:
    """A combo's queue-entry filename. The numeric prefix makes a plain
    lexicographic sort run higher loads (longer sims) first -- the runner
    claims entries in name order, so LPT scheduling falls out for free."""
    return f"{1000 - round(float(load) * 1000):04d}--{exp}--{combo}"


QUEUE_DIRS = ("queue", "claimed", "done", "stolen")


def start_runner(
    host: str,
    root: str,
    cells: list[tuple[str, str, str]],
    slots: int,
    ws: str,
) -> None:
    """Fill the host's stealable queue -- runs/queue/, one prio-named file
    per (experiment, combo, load) cell -- and start the detached runner.py
    (`slots` claim-and-run worker threads; survives SSH disconnect via
    setsid). A rerun replaces any previous runner and clears every queue
    artifact, including pre-work-stealing queue*.list files, so
    progress.py never double-reports."""
    w = root if host == LOCAL else ws
    names = [qname(exp, combo, load) for exp, combo, load in cells]
    dirs = " ".join(f"{w}/runs/{d}" for d in QUEUE_DIRS)
    cleanup = (
        _KILL_OLD.format(w=w)
        + f"; rm -rf {dirs} {w}/runs/queue.stop {w}/runs/queue*.list"
        + f"; mkdir -p {dirs}"
    )
    # The & must background ONE fully-redirected command: with
    # "cd {w} && setsid ... &" the & grabs the whole && list, and that
    # backgrounded subshell keeps sshd's stdout/stderr pipes open while it
    # waits for the runner -- ssh then blocks until TimeoutExpired.
    runner = (
        f"setsid bash -c 'cd {w} && exec python3 scripts/runner.py {w} {slots}' "
        f"</dev/null >{w}/runs/runner.log 2>&1 &"
    )
    if host == LOCAL:
        subprocess.run(["bash", "-c", cleanup], check=False)
        for n in names:
            open(os.path.join(w, "runs", "queue", n), "w").close()
        subprocess.run(["bash", "-c", runner], check=True)
    else:
        _ssh(host, cleanup, check=False)
        _ssh(host, f"cd {w}/runs/queue && touch {' '.join(names)}")
        _ssh(host, runner)
    print(f"runner started on {host}: {len(cells)} combos, {slots} slots")


def probe_slots(hosts: list[str], mem_per_run: int) -> dict[str, int]:
    """Best-effort host -> slots for monitor-time work stealing. Unlike
    probe_all, a host that fails the probe is just left out (it still gets
    polled; only stealing skips it) and nothing is fatal."""

    def one(h: str) -> tuple[str, int]:
        try:
            cpus, mem_gb = probe(h)
            return h, slot_count(cpus, mem_gb, mem_per_run)
        except (RuntimeError, subprocess.TimeoutExpired, ValueError):
            return h, 0

    with concurrent.futures.ThreadPoolExecutor(max(1, len(hosts))) as ex:
        return {h: n for h, n in ex.map(one, hosts) if n > 0}


def plan_steals(
    snaps: dict[str, dict[tuple[str, str], tuple[int, str]]],
    host_slots: dict[str, int],
    exps: list[str],
) -> list[tuple[str, str, str, str, str]]:
    """The work-stealing plan for one poll snapshot, as (exp, combo, load,
    src, dst) moves from backlogged hosts to hosts with idle slots.

    Per host, free = slots - running - queued: a host with free > 0 will
    run dry before the next poll, one with free < 0 holds more queue than
    capacity. Only unclaimed ('q') combos of the monitored experiments
    move, longest (highest-load) first, each from the most backlogged host
    to the freest. Hosts that are unprobed, local, or still carry legacy
    queue.list entries ('-' state: queued and running are indistinguishable)
    are skipped entirely."""
    free: dict[str, int] = {}
    pending: dict[str, list[tuple[str, str]]] = {}
    for h, snap in snaps.items():
        if h == LOCAL or h not in host_slots or not snap:
            continue
        states = [rc for _n, rc in snap.values()]
        if "-" in states:
            continue
        free[h] = host_slots[h] - states.count("run") - states.count("q")
        pending[h] = sorted(
            (ec for ec, (_n, rc) in snap.items() if rc == "q" and ec[0] in exps),
            key=lambda ec: -float(ec[1].rsplit("load", 1)[1]),
        )
    moves = []
    while True:
        tgts = [h for h, f in free.items() if f > 0]
        srcs = [h for h, f in free.items() if f < 0 and pending[h]]
        if not tgts or not srcs:
            return moves
        dst = max(tgts, key=lambda h: free[h])
        src = min(srcs, key=lambda h: free[h])
        exp, combo = pending[src].pop(0)
        moves.append((exp, combo, combo.rsplit("load", 1)[1], src, dst))
        free[src] += 1
        free[dst] -= 1


def rebalance(
    snaps: dict[str, dict[tuple[str, str], tuple[int, str]]],
    host_slots: dict[str, int],
    exps: list[str],
    root: str,
    ws: str,
) -> list[tuple[str, str, str, str]]:
    """Execute the steal plan for one poll and return the moves that
    actually happened, so monitor can update assignments.csv. Per move:
    atomically take the queue entry off src (a runner slot claiming it
    first wins the rename -- the steal is then simply dropped), ship the
    combo's arrivals to dst, and queue it there; dst's idle runner picks it
    up within its recheck interval. A host that fails mid-round is skipped
    for the rest of the round and retried at the next poll."""
    done, dead = [], set()
    for exp, combo, load, src, dst in plan_steals(snaps, host_slots, exps):
        if src in dead or dst in dead:
            continue
        name = qname(exp, combo, load)
        try:
            _ssh(src, f"mv {ws}/runs/queue/{name} {ws}/runs/stolen/{name}")
        except (RuntimeError, subprocess.TimeoutExpired):
            dead.add(src)  # raced by src's own runner, or src unreachable
            continue
        try:
            _ssh(dst, f"mkdir -p {ws}/runs/{exp}/{combo}")
            _rsync(
                dst,
                [os.path.join(root, "runs", exp, combo, "arrivals.csv")],
                f"{ws}/runs/{exp}/{combo}/",
            )
            _ssh(dst, f"touch {ws}/runs/queue/{name}")
        except (RuntimeError, subprocess.TimeoutExpired):
            # put the entry back so the combo is never lost
            _ssh(src, f"mv {ws}/runs/stolen/{name} {ws}/runs/queue/{name}", check=False)
            print(f"WARN steal {exp}/{combo} {src} -> {dst} failed; returned to {src}")
            dead.add(dst)
            continue
        done.append((exp, combo, src, dst))
    return done


def stop_runners(hosts: list[str], root: str, ws: str) -> None:
    """Drop runs/queue.stop on every host so idle runner slots exit instead
    of sleeping forever for steals that will never come (start_runner
    removes the marker again at the next launch)."""
    for h in hosts:
        if h == LOCAL:
            open(os.path.join(root, "runs", "queue.stop"), "w").close()
        else:
            _ssh(h, f"touch {ws}/runs/queue.stop", check=False)


def poll(host: str, root: str, ws: str) -> dict[tuple[str, str], tuple[int, str]]:
    """One progress snapshot of a worker's whole queue:
    (exp, combo) -> (completed_jobs, rc-string or '-')."""
    w = root if host == LOCAL else ws
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


def collect_host(host: str, root: str, exp: str, ws: str) -> None:
    """Pull a worker's results for one experiment back into the local
    runs/<exp>/ combo folders (works mid-run too: occupancy.csv is streamed).
    Only result files come back -- csv/sim.done/run.err; the bulky
    progress.csv / node_jobs.csv (live progress is polled on the worker, not
    from these) and the verbose rotating logs (log.log*, err.log*, jct.log*,
    up to ~100 MB per combo at full trace length) stay on the worker for
    on-demand debugging, and arrivals / the shared jobs symlink dirs never
    move."""
    if host == LOCAL:
        return
    cmd = [
        "rsync",
        "-az",
        "--partial",
        "--exclude=jobs-load*",
        "--exclude=arrivals.csv",
        "--exclude=progress.csv",
        "--exclude=node_jobs.csv",
        "--include=*/",
        "--include=*.csv",
        "--include=sim.done",
        "--include=run.err",
        "--include=failed_nodes.log",
        "--exclude=*",
        "--prune-empty-dirs",
        "-e",
        f"ssh {' '.join(SSH_OPTS)}",
        f"{host}:{ws}/runs/{exp}/",
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
