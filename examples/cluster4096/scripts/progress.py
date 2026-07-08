#!/usr/bin/env python3
"""Per-combo progress of this worker's queue: progress.py <workspace>.

Reads <workspace>/runs/queue.list (the runner's queue) and prints one line
per combo: "<experiment> <combo> <completed_jobs> <rc|->", where
completed_jobs counts the streamed jct.csv rows and rc comes from sim.done
(- while still running). Dependency-free stdlib; runs unchanged on workers.
"""

from __future__ import annotations

import os
import sys


def main() -> int:
    w = sys.argv[1]
    queue = os.path.join(w, "runs", "queue.list")
    if not os.path.isfile(queue):
        return 0
    with open(queue) as f:
        for line in f:
            if not line.strip():
                continue
            exp, combo = line.split()
            d = os.path.join(w, "runs", exp, combo)
            try:
                with open(os.path.join(d, "jct.csv")) as jf:
                    n = max(0, sum(1 for _ in jf) - 1)
            except OSError:
                n = 0
            try:
                rc = open(os.path.join(d, "sim.done")).read().strip()
            except OSError:
                rc = "-"
            print(exp, combo, n, rc)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
