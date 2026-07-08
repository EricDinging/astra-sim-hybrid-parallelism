"""Materialize jobs/<job_id> symlinks from an arrivals.csv shape column.

Each job_id is symlinked to ``<tracelib>/<model>/<shape>``. The same job
sequence is shared by every load (fixed seed), so this runs once per load
directory. It is intentionally dependency-free (no shapes.py / service_times)
so it can run unchanged on each sweep host, where ``--tracelib`` resolves to
the host-local trace library path.
"""

from __future__ import annotations

import argparse
import csv
import os


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="materialize jobs/ symlinks from arrivals.csv"
    )
    ap.add_argument("--arrivals", required=True, help="arrivals.csv path")
    ap.add_argument(
        "--tracelib",
        required=True,
        help="trace library root (contains <model>/<shape>)",
    )
    ap.add_argument("--model", default="bw")
    ap.add_argument("--out", required=True, help="jobs/ output directory")
    args = ap.parse_args(argv)

    lib = os.path.abspath(args.tracelib)
    os.makedirs(args.out, exist_ok=True)
    n = 0
    with open(args.arrivals, newline="") as f:
        for row in csv.DictReader(f):
            jid, shape = row["job_id"], row["shape"]
            target = os.path.join(lib, args.model, shape)
            link = os.path.join(args.out, jid)
            if os.path.islink(link) or os.path.exists(link):
                os.unlink(link)
            os.symlink(target, link)
            n += 1
    print(f"created {n} symlinks under {args.out} -> {lib}/{args.model}/<shape>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
