#!/usr/bin/env python3
"""ckpt.py — checkpoint & resume a fleet of running ASTRA-sim sweeps via CRIU.

Two modes, one script (dependency-free stdlib, like the other farm tooling):

  ckpt.py checkpoint --workers workers.txt --store ./ckpts [--experiment E]
      On every worker: freeze the sweep harness (runner.py/run_combo.py) so
      nothing new starts, then for each running sim send SIGUSR1 (the binary
      sheds its rebuildable caches, flushes logs and SIGSTOPs itself at an
      event-boundary safe point), `criu dump` it (the dump kills the sim),
      kill the frozen harness, and rsync every dumped combo dir — CRIU image,
      CSV outputs, provenance — into the local <store>. After this the hosts
      are quiet and the whole in-flight sweep state lives in <store>.
      --experiment filters which sims are dumped; the harness stop is always
      host-wide (one runner serves every experiment in a workspace).
      --no-pull leaves the images on the workers (manifest written into
      criu-img/) instead of rsyncing them into <store>.
      --combo NAME (or glob) dumps just those combos and leaves the harness alone:
      the sim's run_combo.py parent sees the kill and records rc=-9, the
      other sims keep running. --leave-running snapshots instead of
      evacuating: the sim is dumped at its safe point and SIGCONT'd, its
      CSV outputs are copied while it is still paused so the store holds an
      image-consistent set, and the harness is never touched.

  ckpt.py resume --workers workers.txt --store ./ckpts
      Distribute the not-yet-resumed checkpoints in <store> round-robin over
      the workers (any deployed hosts — not necessarily the original ones),
      push each combo dir back to its original absolute path, verify the
      deployed binary matches the dumped binary's md5, rebuild the jobs dir
      (via run_combo.build_jobs_dir, lock and all) if missing, `criu restore`
      + SIGCONT, and leave a babysitter that writes sim.done (rc=0 on a clean
      finish) when the restored sim exits. Already-resumed combos are
      skipped, so rerunning after partial failures retries only what's left.

Requirements/invariants (same deploy invariants the sweeps rely on):
  * binary with the SIGUSR1 safe point, deployed at the same absolute path
    everywhere, byte-identical (md5-verified here);
  * workspace layout identical across hosts (tracelib/configs/scripts), since
    CRIU restores open fds by absolute path;
  * criu >= 3.17 (jammy's stock 3.16.1 segfaults on restore — no glibc-2.35
    rseq support); this script auto-installs 4.x from ppa:criu/ppa;
  * passwordless sudo on the workers (CloudLab default).

Caveats:
  * criu restores the sim under its original PID; if that PID is taken on the
    target host the restore fails cleanly — rerun `resume`, or point it at a
    less loaded worker.
  * run_combo.py skips a combo whose restored sim is still in flight (it
    checks criu-img/restored.at), so relaunching a sweep on a resumed host is
    safe — but the resumed combos stay owned by their babysitters, not the
    sweep harness.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import fnmatch
import glob
import json
import os
import shlex
import subprocess
import sys

import launcher
from run_combo import BIN_COMM

# ControlMaster: dump/restore issue several short SSH commands per combo, so
# multiplex them over one connection per host instead of a handshake each.
SSH_OPTS = [
    *launcher.SSH_OPTS,
    "-o",
    "ControlMaster=auto",
    "-o",
    "ControlPath=~/.ssh/ckpt-%r@%h:%p",
    "-o",
    "ControlPersist=60",
]
LOCAL = "local"
# Safe-point stop wait. Event boundaries are ~instant on an idle host, but
# a 16 GiB rfold sim on a swap-bound host took >2 min to shed its caches
# and flush (2026-09-03, two sims left paused by the old 120 s limit).
STOP_TIMEOUT_S = 1800
MANIFEST = "ckpt_manifest.json"


def sh(host: str, cmd: str, check: bool = True, timeout: int = 600) -> str:
    """Run `cmd` through bash on `host` ('local' = this box)."""
    argv = ["bash", "-c", cmd] if host == LOCAL else ["ssh", *SSH_OPTS, host, cmd]
    r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError(f"[{host}] `{cmd[:80]}...`: {r.stderr.strip()[-300:]}")
    return r.stdout


def rsync(src: str, dst: str) -> None:
    subprocess.run(
        ["rsync", "-az", "--partial", "-e", f"ssh {' '.join(SSH_OPTS)}", src, dst],
        check=True,
        capture_output=True,
    )


def loc(host: str, path: str) -> str:
    """rsync location spec for a path on a host."""
    return path if host == LOCAL else f"{host}:{path}"


def parse_workers(path: str) -> list[str]:
    hosts = launcher.parse_workers(path)
    if not hosts:
        # Unlike launcher (empty => run locally), checkpointing an implicit
        # host is surprising; require 'local' to be listed explicitly.
        raise SystemExit(f"no workers in {path}")
    return hosts


def ensure_criu(host: str) -> None:
    """criu >= 3.17 or install 4.x from the CRIU PPA."""
    sh(
        host,
        'v=$(criu --version 2>/dev/null | awk "/^Version/{print \\$2}"); '
        'case "$v" in ""|[12].*|3.[0-9]|3.[0-9].*|3.1[0-6]*) '
        "sudo add-apt-repository -y ppa:criu/ppa >/dev/null 2>&1; "
        "sudo apt-get -qq update >/dev/null && sudo apt-get -qq install -y criu >/dev/null;; "
        "esac; criu --version >/dev/null",
        timeout=300,
    )


def running_sims(
    host: str, experiment: str | None, combo: str | None = None
) -> list[dict]:
    """[{pid, combo_dir, jobs_dir}] of live sims on the host."""
    out = sh(host, "ps -eo pid,comm,args --no-headers", check=False)
    sims = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) != 3 or parts[1] != BIN_COMM:
            continue
        combo_dir = jobs_dir = None
        for tok in parts[2].split():
            if tok.startswith("--logging-folder="):
                combo_dir = tok.split("=", 1)[1]
            elif tok.startswith("--jobs-dir="):
                jobs_dir = tok.split("=", 1)[1]
        if not combo_dir:
            continue
        if experiment and f"/runs/{experiment}/" not in combo_dir + "/":
            continue
        if combo and not fnmatch.fnmatch(os.path.basename(combo_dir), combo):
            continue
        sims.append(
            {"pid": int(parts[0]), "combo_dir": combo_dir, "jobs_dir": jobs_dir}
        )
    return sims


def signal_harness(host: str, sig: str) -> None:
    """STOP/KILL/CONT every runner.py/run_combo.py on the host. Host-wide by
    design: one runner serves a whole workspace, so an experiment-scoped
    freeze would leave it claiming new combos mid-checkpoint."""
    sh(
        host,
        f"pkill -{sig} -f 'runner\\.py|run_combo\\.py' || true",
        check=False,
    )


def dump_one(host: str, sim: dict, leave_running: bool = False) -> dict:
    """Safe-point stop + criu dump one sim; returns provenance for the manifest.
    leave_running: criu -R, then copy the outputs into criu-img/outputs/
    while the sim is still paused and SIGCONT it."""
    pid, combo_dir = sim["pid"], sim["combo_dir"]
    img = f"{combo_dir}/criu-img"
    # SIGUSR1 -> wait for the self-SIGSTOP at the safe point.
    sh(
        host,
        f"kill -USR1 {pid}; "
        f"for _ in $(seq 1 {STOP_TIMEOUT_S}); do "
        f"  s=$(ps -o stat= -p {pid} 2>/dev/null) "
        f'    || {{ echo "sim {pid} exited before the safe point" >&2; exit 1; }}; '
        f'  case "$s" in T*) exit 0;; esac; sleep 1; '
        # Never leave the sim paused: CONT it. If its self-SIGSTOP lands
        # after this, it stays paused until someone CONTs it by hand.
        f"done; kill -CONT {pid}; "
        f'echo "sim {pid} not stopped after {STOP_TIMEOUT_S}s (CONT sent)" >&2; exit 1',
        timeout=STOP_TIMEOUT_S + 60,
    )
    # A sim that was itself restored earlier has a babysitter polling its pid
    # (resume_one); kill it or it would write sim.done rc=97 the moment the
    # dump kills the sim. The [0] keeps the pattern from matching this very
    # pkill's own command line.
    if not leave_running:
        sh(host, f"pkill -f {shlex.quote(f'kill -[0] {pid} ')} || true", check=False)
    prov = sh(
        host,
        f"set -eo pipefail; rm -rf {shlex.quote(img)}; mkdir -p {shlex.quote(img)}; "
        f"tr '\\0' '\\n' < /proc/{pid}/cmdline > {shlex.quote(img)}/cmdline; "
        # A binary rebuilt under a running sim leaves exe pointing at a deleted
        # inode: hash the mapped file via /proc, and drop the ' (deleted)' tag.
        f"exe=$(readlink -f /proc/{pid}/exe); exe=${{exe%% (deleted)}}; "
        f"cwd=$(readlink -f /proc/{pid}/cwd); "
        f'md5=$(md5sum /proc/{pid}/exe | cut -d" " -f1); '
        f'echo "$exe"; echo "$md5"; echo "$cwd"',
    ).splitlines()
    exe, md5, cwd = prov[0].strip(), prov[1].strip(), prov[2].strip()
    try:
        sh(
            host,
            # --shell-job: sweep sims run under run_combo.py, so they are not
            # session leaders; their session/pgroup leader lives outside the
            # dumped tree.
            # --ghost-limit: the sim may map a binary that was rebuilt (unlinked)
            # after it started; CRIU must copy the ~100 MB ghost into the image.
            f"sudo criu dump -t {pid} -D {shlex.quote(img)} --shell-job "
            f"--ghost-limit 256M {'-R ' if leave_running else ''}-o dump.log && "
            f'sudo chown -R "$(id -un):" {shlex.quote(img)}',
            timeout=1800,
        )
    except RuntimeError:
        sh(
            host, f"kill -CONT {pid} 2>/dev/null || true", check=False
        )  # sim keeps running
        tail = sh(host, f"sudo tail -3 {shlex.quote(img)}/dump.log", check=False)
        raise RuntimeError(
            f"criu dump failed for {combo_dir} (sim resumed): {tail.strip()}"
        )
    if leave_running:
        # Outputs must match the image: copy them before the sim moves on.
        # arrivals.csv is static input and is pulled separately.
        sh(
            host,
            f"set -e; cd {shlex.quote(combo_dir)}; mkdir criu-img/outputs; "
            "find . -maxdepth 1 -type f ! -name arrivals.csv "
            "-exec cp -p {} criu-img/outputs/ \\; ; "
            f"kill -CONT {pid}",
        )
    return {
        "leave_running": leave_running,
        "host": host,
        "combo_dir": combo_dir,
        "jobs_dir": sim["jobs_dir"],
        "binary": exe,
        "binary_md5": md5,
        "cwd": cwd,
        "pid": pid,
        "dumped_at": datetime.datetime.now().isoformat(timespec="seconds"),
    }


def pull_one(host: str, store: str, m: dict) -> str:
    combo = os.path.basename(m["combo_dir"])
    exp = os.path.basename(os.path.dirname(m["combo_dir"]))
    dst = os.path.join(store, exp, combo)
    os.makedirs(dst, exist_ok=True)
    if m.get("leave_running"):
        # The live sim keeps appending to its outputs; take the paused-time
        # copies from the image dir instead of the combo dir's current files.
        rsync(loc(host, m["combo_dir"] + "/criu-img/"), dst + "/criu-img/")
        rsync(loc(host, m["combo_dir"] + "/arrivals.csv"), dst + "/")
        outs = os.path.join(dst, "criu-img", "outputs")
        for f in os.listdir(outs):
            os.replace(os.path.join(outs, f), os.path.join(dst, f))
        os.rmdir(outs)
    else:
        rsync(loc(host, m["combo_dir"] + "/"), dst + "/")
    with open(os.path.join(dst, MANIFEST), "w") as f:
        json.dump(m, f, indent=1)
    return f"{host}: pulled {exp}/{combo} -> {dst}"


def checkpoint_host(
    host: str,
    experiment: str | None,
    store: str,
    combo: str | None = None,
    leave_running: bool = False,
    pull: bool = True,
) -> tuple[list[str], int]:
    """Dump every running sim on `host`, pull to the store: (report, nfail).
    Only a full-host kill-mode dump is an evacuation that freezes and then
    kills the harness; a single combo or a live snapshot leaves it alone."""
    lines, fails = [], 0
    sims = running_sims(host, experiment, combo)
    if not sims:
        return [f"{host}: no running sims"], 0
    ensure_criu(host)
    evacuate = not combo and not leave_running
    if evacuate:
        signal_harness(host, "STOP")  # nothing new may start or react
    dumped = []
    for sim in sims:  # sequential: parallel criu dumps thrash the same disk
        try:
            dumped.append(dump_one(host, sim, leave_running))
            lines.append(
                f"{host}: dumped {os.path.basename(sim['combo_dir'])} (pid {sim['pid']})"
            )
        except (RuntimeError, subprocess.TimeoutExpired) as e:
            lines.append(f"{host}: FAIL {sim['combo_dir']}: {e}")
            fails += 1
    if not evacuate:
        pass
    elif dumped:
        signal_harness(host, "KILL")  # host is quiet now
    else:
        # Every dump failed and every sim was SIGCONT'd — put the harness
        # back the way we found it instead of orphaning live sims.
        signal_harness(host, "CONT")
    if not pull:
        for m in dumped:  # manifest next to the image, for a later pull/resume
            sh(
                host,
                f"cat > {shlex.quote(m['combo_dir'] + '/criu-img/' + MANIFEST)}"
                f" <<'EOF'\n{json.dumps(m, indent=1)}\nEOF",
            )
        return lines, fails
    # Pulls are independent and network-bound; overlap a few per host.
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
        futs = {ex.submit(pull_one, host, store, m): m for m in dumped}
        for fut in concurrent.futures.as_completed(futs):
            try:
                lines.append(fut.result())
            except (OSError, subprocess.SubprocessError) as e:
                m = futs[fut]
                # Image is dumped on the host but not in the store; rerunning
                # checkpoint won't refetch it (the sim is dead), so pull by hand.
                lines.append(
                    f"{host}: FAIL pull {m['combo_dir']} ({e}) — "
                    f"image left at {m['combo_dir']}/criu-img"
                )
                fails += 1
    return lines, fails


def pending_checkpoints(store: str) -> list[str]:
    """Combo dirs in the store that have an image and were not yet resumed."""
    out = []
    for mf in sorted(glob.glob(os.path.join(store, "*", "*", MANIFEST))):
        d = os.path.dirname(mf)
        with open(mf) as f:
            resumed = json.load(f).get("resumed_ok")
        if not resumed and os.path.isfile(os.path.join(d, "criu-img", "inventory.img")):
            out.append(d)
    return out


def resume_one(host: str, local_dir: str) -> str:
    with open(os.path.join(local_dir, MANIFEST)) as f:
        m = json.load(f)
    combo_dir, img = m["combo_dir"], m["combo_dir"] + "/criu-img"
    q = shlex.quote
    # The deployed binary must be byte-identical to the dumped one.
    got = sh(host, f'md5sum {q(m["binary"])} | cut -d" " -f1').strip()
    if got != m["binary_md5"]:
        raise RuntimeError(
            f"binary mismatch on {host}: {got} != {m['binary_md5']} (redeploy first)"
        )
    sh(host, f"mkdir -p {q(os.path.dirname(combo_dir))} {q(m['cwd'])}")
    rsync(local_dir + "/", loc(host, combo_dir + "/"))
    # Jobs dir (open .et fds resolve here): rebuild through run_combo's
    # builder so the flock and .complete marker guard against concurrent
    # sweeps racing the same shared jobs-load<L> dir. Must run after the
    # combo push above — it reads the combo's arrivals.csv. Gate on the
    # .complete marker, not bare existence: a dir left by a crashed builder
    # exists but is unusable.
    if (
        m["jobs_dir"]
        and not sh(
            host, f"test -f {q(m['jobs_dir'] + '/.complete')} && echo y", check=False
        ).strip()
    ):
        root = os.path.dirname(os.path.dirname(os.path.dirname(combo_dir)))
        exp = os.path.basename(os.path.dirname(combo_dir))
        load = os.path.basename(m["jobs_dir"]).removeprefix("jobs-load")
        py = (
            f"import sys; sys.path.insert(0, {root + '/scripts'!r}); "
            f"import run_combo; "
            f"run_combo.build_jobs_dir({root!r}, {exp!r}, {combo_dir!r}, {load!r})"
        )
        sh(host, f"python3 -c {q(py)}", timeout=1800)
    # A stale sim.done would make sweep tooling misread the combo; the
    # babysitter below rewrites it when the restored sim actually exits.
    sh(
        host,
        f"set -e; rm -f {q(combo_dir)}/sim.done; touch {q(img)}/restored.at; "
        f"sudo criu restore -D {q(img)} -d --shell-job -o restore.log "
        f"|| {{ sudo tail -3 {q(img)}/restore.log >&2; exit 1; }}",
        timeout=1800,
    )
    pid = m["pid"]
    sh(host, f"kill -CONT {pid}")
    # Babysitter: run_combo.py no longer wraps this sim, so write sim.done
    # ourselves when it exits (rc=0 iff summary.txt was written after restore).
    baby = (
        f"while kill -0 {pid} 2>/dev/null; do sleep 30; done; "
        f"if [ {q(combo_dir)}/summary.txt -nt {q(img)}/restored.at ]; "
        f"then echo rc=0 > {q(combo_dir)}/sim.done; "
        f"else echo rc=97 > {q(combo_dir)}/sim.done; fi"
    )
    sh(host, f"nohup setsid bash -c {q(baby)} >/dev/null 2>&1 & true", check=False)
    m.update(
        resumed_to=host,
        resumed_at=datetime.datetime.now().isoformat(timespec="seconds"),
        resumed_ok=True,
    )
    with open(os.path.join(local_dir, MANIFEST), "w") as f:
        json.dump(m, f, indent=1)
    return f"{host}: resumed {os.path.basename(combo_dir)} (pid {pid})"


def cmd_checkpoint(args: argparse.Namespace) -> int:
    hosts = parse_workers(args.workers)
    os.makedirs(args.store, exist_ok=True)
    fails = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as ex:
        for lines, nfail in ex.map(
            lambda h: checkpoint_host(
                h,
                args.experiment,
                args.store,
                args.combo,
                args.leave_running,
                not args.no_pull,
            ),
            hosts,
        ):
            print("\n".join(lines))
            fails += nfail
    return 1 if fails else 0


def cmd_resume(args: argparse.Namespace) -> int:
    hosts = parse_workers(args.workers)
    pend = pending_checkpoints(args.store)
    if not pend:
        print(f"nothing to resume in {args.store}")
        return 0
    print(f"{len(pend)} checkpoint(s) over {len(hosts)} worker(s)")
    jobs = [(hosts[i % len(hosts)], d) for i, d in enumerate(pend)]
    used = sorted({h for h, _ in jobs})
    fails = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as ex:
        # criu install/check once per host, not once per checkpoint.
        for host, err in zip(used, ex.map(lambda h: _try(ensure_criu, h), used)):
            if err:
                print(f"{host}: FAIL criu setup: {err}")
                jobs = [(h, d) for h, d in jobs if h != host]
                fails += 1
        futs = {ex.submit(resume_one, h, d): (h, d) for h, d in jobs}
        for fut in concurrent.futures.as_completed(futs):
            h, d = futs[fut]
            try:
                print(fut.result())
            except (RuntimeError, subprocess.SubprocessError) as e:
                print(f"{h}: FAIL {os.path.basename(d)}: {e}")
                fails += 1
    if fails:
        print(f"{fails} failed — rerun `resume` to retry them (manifests unmarked)")
    return 1 if fails else 0


def _try(fn, *a):
    try:
        fn(*a)
        return None
    except (RuntimeError, subprocess.SubprocessError) as e:
        return e


def main() -> int:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--workers", required=True, help="workers file (user@host per line, or 'local')"
    )
    common.add_argument(
        "--store", required=True, help="local checkpoint store directory"
    )
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("checkpoint", parents=[common])
    c.add_argument(
        "--experiment", default=None, help="only sims of this experiment (default: all)"
    )
    c.add_argument("--combo", default=None, help="only combos matching this glob")
    c.add_argument(
        "--no-pull",
        action="store_true",
        help="keep images on the workers; skip the rsync into --store",
    )
    c.add_argument(
        "--leave-running",
        action="store_true",
        help="snapshot: dump, then SIGCONT the sim instead of killing it",
    )
    c.set_defaults(fn=cmd_checkpoint)
    r = sub.add_parser("resume", parents=[common])
    r.set_defaults(fn=cmd_resume)
    args = p.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
