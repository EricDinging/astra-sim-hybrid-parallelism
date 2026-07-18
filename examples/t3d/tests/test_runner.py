"""Self-running tests for scripts/runner.py (stealable per-host queue) and
scripts/progress.py (queue-dir progress states), against a stub workspace."""

import os
import subprocess
import sys
import tempfile

SCRIPTS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
)
sys.path.insert(0, SCRIPTS)

import launcher  # noqa: E402

# stub run_combo.py: records its (exp, combo) invocation and writes sim.done
STUB_RUN_COMBO = """\
import os, sys
w, exp, combo = sys.argv[1], sys.argv[2], sys.argv[3]
d = os.path.join(w, "runs", exp, combo)
os.makedirs(d, exist_ok=True)
with open(os.path.join(d, "progress.csv"), "w") as f:
    f.write("header\\nrow\\n")
with open(os.path.join(d, "sim.done"), "w") as f:
    f.write("rc=0\\n")
"""


def make_workspace(d: str, combos: list[tuple[str, str, str]]) -> None:
    for sub in launcher.QUEUE_DIRS:
        os.makedirs(os.path.join(d, "runs", sub))
    os.makedirs(os.path.join(d, "scripts"))
    with open(os.path.join(d, "scripts", "run_combo.py"), "w") as f:
        f.write(STUB_RUN_COMBO)
    for exp, combo, load in combos:
        name = launcher.qname(exp, combo, load)
        open(os.path.join(d, "runs", "queue", name), "w").close()


def test_runner_drains_queue_and_retires_entries():
    combos = [
        ("e1", "easy-firstfit-load0.90", "0.90"),
        ("e1", "easy-rfold-load0.50", "0.50"),
        ("e2", "swf-sfc-load0.05", "0.05"),
    ]
    with tempfile.TemporaryDirectory() as d:
        make_workspace(d, combos)
        # stop marker pre-dropped: slots drain the queue, then exit
        open(os.path.join(d, "runs", "queue.stop"), "w").close()
        subprocess.run(
            [sys.executable, os.path.join(SCRIPTS, "runner.py"), d, "2"],
            check=True,
            timeout=60,
        )
        assert os.listdir(os.path.join(d, "runs", "queue")) == []
        assert os.listdir(os.path.join(d, "runs", "claimed")) == []
        assert len(os.listdir(os.path.join(d, "runs", "done"))) == 3
        for exp, combo, _ in combos:
            done = os.path.join(d, "runs", exp, combo, "sim.done")
            assert open(done).read().strip() == "rc=0"


def test_runner_marks_crashed_combo():
    with tempfile.TemporaryDirectory() as d:
        make_workspace(d, [("e1", "easy-firstfit-load0.10", "0.10")])
        # a run_combo that dies without writing sim.done
        with open(os.path.join(d, "scripts", "run_combo.py"), "w") as f:
            f.write("raise SystemExit(1)\n")
        open(os.path.join(d, "runs", "queue.stop"), "w").close()
        subprocess.run(
            [sys.executable, os.path.join(SCRIPTS, "runner.py"), d, "1"],
            check=True,
            timeout=60,
        )
        done = os.path.join(d, "runs", "e1", "easy-firstfit-load0.10", "sim.done")
        assert open(done).read().strip() == "rc=97"


def progress_lines(d: str) -> dict[tuple[str, str], tuple[str, str]]:
    out = subprocess.run(
        [sys.executable, os.path.join(SCRIPTS, "progress.py"), d],
        check=True,
        capture_output=True,
        text=True,
        timeout=60,
    ).stdout
    snap = {}
    for line in out.splitlines():
        exp, combo, n, rc = line.split()
        snap[(exp, combo)] = (n, rc)
    return snap


def test_progress_reports_queue_states_and_legacy_list():
    with tempfile.TemporaryDirectory() as d:
        make_workspace(d, [("e1", "easy-firstfit-load0.90", "0.90")])
        runs = os.path.join(d, "runs")
        # claimed entry, done entry (with sim.done), and a legacy queue.list
        open(
            os.path.join(runs, "claimed", launcher.qname("e1", "a-b-load0.50", "0.50")),
            "w",
        ).close()
        os.makedirs(os.path.join(runs, "e1", "a-b-load0.30"))
        with open(os.path.join(runs, "e1", "a-b-load0.30", "sim.done"), "w") as f:
            f.write("rc=0\n")
        open(
            os.path.join(runs, "done", launcher.qname("e1", "a-b-load0.30", "0.30")),
            "w",
        ).close()
        with open(os.path.join(runs, "queue.list"), "w") as f:
            f.write("old old-ff-load0.20\n")
        snap = progress_lines(d)
        assert snap[("e1", "easy-firstfit-load0.90")][1] == "q"
        assert snap[("e1", "a-b-load0.50")][1] == "run"
        assert snap[("e1", "a-b-load0.30")][1] == "rc=0"
        assert snap[("old", "old-ff-load0.20")][1] == "-"


if __name__ == "__main__":
    fns = [
        v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)
    ]
    failed = 0
    for fn in fns:
        try:
            fn()
            print(f"PASS {fn.__name__}")
        except Exception as exc:  # noqa: BLE001
            failed += 1
            print(f"FAIL {fn.__name__}: {exc}")
    print(f"{len(fns) - failed}/{len(fns)} passed")
    sys.exit(1 if failed else 0)
