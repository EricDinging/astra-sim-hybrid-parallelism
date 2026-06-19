#!/usr/bin/env python3
"""pool.py: completion, failure surfacing, per-host slot cap, idempotent rerun."""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
POOL = os.path.join(HERE, "pool.py")

HANDLER = """#!/bin/bash
# args: HOST ITEMNAME — record concurrency, sleep, fail on demand
host=$1; name=$2
trace="$TRACE_DIR/$host.trace"
flock "$trace.lock" bash -c "echo start $$ >> '$trace'"
sleep 0.3
flock "$trace.lock" bash -c "echo end $$ >> '$trace'"
[[ "$name" == FAILME* ]] && exit 1
exit 0
"""


def max_overlap(trace):
    depth = peak = 0
    with open(trace) as f:
        for line in f:
            depth += 1 if line.startswith("start") else -1
            peak = max(peak, depth)
    return peak


def run_pool(tmp, items_name="items.txt"):
    env = dict(os.environ, TRACE_DIR=tmp)
    return subprocess.run(
        [
            sys.executable,
            POOL,
            "--hosts",
            "hostA hostB",
            "--slots",
            "2",
            "--items",
            os.path.join(tmp, items_name),
            "--handler",
            os.path.join(tmp, "handler.sh"),
            "--log-dir",
            os.path.join(tmp, "logs"),
        ],
        capture_output=True,
        text=True,
        env=env,
    )


def main():
    tmp = tempfile.mkdtemp(prefix="test_pool_")
    try:
        with open(os.path.join(tmp, "handler.sh"), "w") as f:
            f.write(HANDLER)
        items = [f"job{i}" for i in range(10)] + ["FAILME1", "FAILME2"]
        with open(os.path.join(tmp, "items.txt"), "w") as f:
            f.write("\n".join(items) + "\n")
        r = run_pool(tmp)
        assert r.returncode == 1, r.stdout + r.stderr  # failures surfaced
        logs = os.path.join(tmp, "logs")
        done = set(open(os.path.join(logs, "done.txt")).read().splitlines())
        failed = set(open(os.path.join(logs, "failed.txt")).read().splitlines())
        assert {f"job{i}" for i in range(10)} <= done
        assert {"FAILME1", "FAILME2"} <= failed and not failed & done
        for h in ("hostA", "hostB"):  # never >2 concurrent per host
            tr = os.path.join(tmp, f"{h}.trace")
            assert os.path.isfile(tr), f"host {h} never used"
            assert max_overlap(tr) <= 2, f"{h} overlap {max_overlap(tr)}"
        # rerun: only the failed items run again
        r2 = run_pool(tmp)
        assert "2 to run" in r2.stdout, r2.stdout
        print("PASS pool.py")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
