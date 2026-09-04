"""Self-running tests for scripts/run_combo.py milestone snapshots: babysit()
against a stub sim (appends progress rows, SIGUSR1 -> flush + self-SIGSTOP
like the binary) with criu replaced by a stub via $CRIU. Needs no root."""

import os
import stat
import subprocess
import sys
import tempfile

SCRIPTS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
)
sys.path.insert(0, SCRIPTS)

STUB_SIM = """\
import os, signal, sys, time
out = sys.argv[1]
signal.signal(signal.SIGUSR1, lambda *_: os.kill(os.getpid(), signal.SIGSTOP))
with open(out, "w") as f:
    f.write("header\\n")
    for i in range(int(sys.argv[2])):
        f.write(f"{i}\\n"); f.flush(); time.sleep(0.02)
"""
# records the dumped pid and lays down an inventory.img like criu would
STUB_CRIU = """\
#!/bin/bash
d=""; while [ $# -gt 0 ]; do [ "$1" = -D ] && d=$2; [ "$1" = -t ] && t=$2; shift; done
[ -n "$d" ] || exit 2
echo "$t $(ps -o stat= -p $t)" > "$d/inventory.img"
"""


def test_babysit_snapshots_each_milestone_and_keeps_latest():
    with tempfile.TemporaryDirectory() as d:
        os.environ["CRIU"] = os.path.join(d, "criu")
        with open(os.environ["CRIU"], "w") as f:
            f.write(STUB_CRIU)
        os.chmod(os.environ["CRIU"], stat.S_IRWXU)
        import run_combo  # after CRIU is set: it is read at import

        combo = os.path.join(d, "runs", "e", "c")
        os.makedirs(combo)
        with open(os.path.join(combo, "arrivals.csv"), "w") as f:
            f.write("static input\n")
        prog = os.path.join(combo, "progress.csv")
        proc = subprocess.Popen(
            [sys.executable, "-c", STUB_SIM, prog, "60"],
            cwd=d,
            stdin=subprocess.DEVNULL,
        )
        seen = []
        real = run_combo.snapshot

        def spy(pid, cd, runs, jobs):
            real(pid, cd, runs, jobs)
            seen.append(jobs)

        run_combo.snapshot = spy
        rc = run_combo.babysit(proc, combo, os.path.join(d, "runs"), (20, 40), 0.05)
        assert rc == 0
        assert seen == [20, 40], seen
        img = os.path.join(combo, "criu-img")
        assert not os.path.exists(img + ".new")
        pid, st = open(os.path.join(img, "inventory.img")).read().split()
        assert int(pid) == proc.pid and st.startswith("T"), (pid, st)  # dumped paused
        # paused-time output copy exists and is a prefix of the final file
        copy = open(os.path.join(img, "outputs", "progress.csv")).read()
        assert 40 <= copy.count("\n") - 1 < 60 and open(prog).read().startswith(copy)
        assert not os.path.exists(os.path.join(img, "outputs", "arrivals.csv"))
        import json

        m = json.load(open(os.path.join(img, "ckpt_manifest.json")))
        assert m["jobs"] == 40 and m["pid"] == proc.pid and m["leave_running"]
        assert m["binary"] == os.path.realpath(sys.executable) or m["binary_md5"]


def test_babysit_survives_failed_dump():
    with tempfile.TemporaryDirectory() as d:
        os.environ["CRIU"] = "/bin/false"
        import importlib

        import run_combo

        importlib.reload(run_combo)
        combo = os.path.join(d, "runs", "e", "c")
        os.makedirs(combo)
        prog = os.path.join(combo, "progress.csv")
        proc = subprocess.Popen(
            [sys.executable, "-c", STUB_SIM, prog, "30"],
            cwd=d,
            stdin=subprocess.DEVNULL,
        )
        rc = run_combo.babysit(proc, combo, os.path.join(d, "runs"), (10,), 0.05)
        assert rc == 0  # sim was SIGCONT'd and finished
        assert not os.path.exists(os.path.join(combo, "criu-img"))
        assert sum(1 for _ in open(prog)) == 31


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
