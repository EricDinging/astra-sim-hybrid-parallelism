#!/usr/bin/env python3
"""Per-host sweep runner: runner.py <workspace> <slots>.

Replaces the old xargs-over-queue.list runner with a stealable queue. The
launch phase fills runs/queue/ with one empty file per assigned combo,
named <prio>--<experiment>--<combo> (prio sorts higher loads = longer sims
first, see launcher.qname). Each of the <slots> worker threads claims the
first entry by renaming it into runs/claimed/ -- rename is atomic, so a
sibling slot or a driver-side steal (monitor moving the entry to another
host) simply wins the race -- runs run_combo.py on it, and retires the
entry into runs/done/ (progress.py reads all three dirs). An idle slot
keeps rechecking the queue so work stolen *to* this host mid-sweep gets
picked up; it exits once the driver drops runs/queue.stop at the end of
the sweep. Dependency-free stdlib; runs unchanged on workers.
"""

from __future__ import annotations

import os
import subprocess
import sys
import threading
import time

IDLE_RECHECK_SECS = 30


def main() -> int:
    w = os.path.abspath(sys.argv[1])
    slots = int(sys.argv[2])
    runs = os.path.join(w, "runs")

    def claim() -> str | None:
        """Atomically move the first queue entry into claimed/; None when
        the queue is empty (or every entry was raced away)."""
        for name in sorted(os.listdir(os.path.join(runs, "queue"))):
            try:
                os.rename(
                    os.path.join(runs, "queue", name),
                    os.path.join(runs, "claimed", name),
                )
                return name
            except OSError:
                continue
        return None

    def slot() -> None:
        while True:
            name = claim()
            if name is None:
                if os.path.exists(os.path.join(runs, "queue.stop")):
                    return
                time.sleep(IDLE_RECHECK_SECS)
                continue
            _prio, exp, combo = name.split("--")
            subprocess.run(
                [
                    sys.executable,
                    os.path.join(w, "scripts", "run_combo.py"),
                    w,
                    exp,
                    combo,
                ],
                check=False,
            )
            done = os.path.join(runs, exp, combo, "sim.done")
            if not os.path.isfile(done):  # run_combo died before writing it
                os.makedirs(os.path.dirname(done), exist_ok=True)
                with open(done, "w") as f:
                    f.write("rc=97\n")
            os.rename(
                os.path.join(runs, "claimed", name), os.path.join(runs, "done", name)
            )

    threads = [threading.Thread(target=slot) for _ in range(slots)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
