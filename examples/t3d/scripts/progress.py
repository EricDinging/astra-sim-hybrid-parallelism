#!/usr/bin/env python3
"""Per-combo progress of this worker's queue: progress.py <workspace>.

Reads <workspace>/runs/queue*.list (the per-memory-class runner queues, plus
a legacy single queue.list from pre-tiering launches) and prints one line
per combo: "<experiment> <combo> <completed_jobs> <rc|->", where
completed_jobs counts the streamed progress.csv rows (falling back to the
old jct.csv name for runs started before the rename) and rc comes from
sim.done (- while still running). Dependency-free stdlib; runs unchanged on
workers.
"""

from __future__ import annotations

import glob
import os
import sys


def main() -> int:
    w = sys.argv[1]
    seen = set()
    for queue in sorted(glob.glob(os.path.join(w, "runs", "queue*.list"))):
        with open(queue) as f:
            for line in f:
                if not line.strip():
                    continue
                exp, combo = line.split()
                if (exp, combo) in seen:
                    continue
                seen.add((exp, combo))
                d = os.path.join(w, "runs", exp, combo)
                n = 0
                for name in ("progress.csv", "jct.csv"):
                    try:
                        with open(os.path.join(d, name)) as jf:
                            n = max(0, sum(1 for _ in jf) - 1)
                        break
                    except OSError:
                        pass
                try:
                    rc = open(os.path.join(d, "sim.done")).read().strip()
                except OSError:
                    rc = "-"
                print(exp, combo, n, rc)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
