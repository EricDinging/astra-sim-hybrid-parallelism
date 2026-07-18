#!/usr/bin/env python3
"""Per-combo progress of this worker's queue: progress.py <workspace>.

Reads the runner's queue dirs (runs/queue|claimed|done, entries named
<prio>--<experiment>--<combo>) plus any legacy runs/queue*.list from
pre-work-stealing launches, and prints one line per combo:
"<experiment> <combo> <completed_jobs> <state>". completed_jobs counts the
streamed progress.csv rows (falling back to the old jct.csv name); state is
q (queued, stealable) / run (claimed by a runner slot) / the sim.done rc
once finished / - (legacy entry not finished: the old format can't tell
queued from running). Dependency-free stdlib; runs unchanged on workers.
"""

from __future__ import annotations

import glob
import os
import sys


def jobs_done(combo_dir: str) -> int:
    for name in ("progress.csv", "jct.csv"):
        try:
            with open(os.path.join(combo_dir, name)) as f:
                return max(0, sum(1 for _ in f) - 1)
        except OSError:
            pass
    return 0


def main() -> int:
    w = sys.argv[1]
    runs = os.path.join(w, "runs")
    seen = set()

    def emit(exp: str, combo: str, state: str | None) -> None:
        if (exp, combo) in seen:
            return
        seen.add((exp, combo))
        d = os.path.join(runs, exp, combo)
        if state is None:
            try:
                state = open(os.path.join(d, "sim.done")).read().strip()
            except OSError:
                state = "-"
        print(exp, combo, jobs_done(d), state)

    # active queue states win over a stale done/ entry for the same combo
    for sub, state in (("queue", "q"), ("claimed", "run"), ("done", None)):
        try:
            names = sorted(os.listdir(os.path.join(runs, sub)))
        except OSError:
            names = []
        for name in names:
            parts = name.split("--")
            if len(parts) == 3:
                emit(parts[1], parts[2], state)
    for queue in sorted(glob.glob(os.path.join(runs, "queue*.list"))):  # legacy
        with open(queue) as f:
            for line in f:
                if line.strip():
                    exp, combo = line.split()
                    emit(exp, combo, None)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
