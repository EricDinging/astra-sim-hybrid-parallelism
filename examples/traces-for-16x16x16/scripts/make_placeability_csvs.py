#!/usr/bin/env python3
"""Aggregate placeability-probe raw output into per-failure-rate CSVs.

One CSV per swept rate: placeability_16x16x16.csv for the idle cluster
(rate 0) and placeability_16x16x16_fail<P>pct.csv for failure rate P%.
Columns: a,b,c,dim,<one y/n column per policy>. "y" means the policy's
try_place / placeable-on-idle oracle places the shape on the degraded-idle
torus; DROP, DEFER (snake-budget artifacts that never place in practice)
and NOPE all map to "n".

Normally invoked by run_placeability_census.sh.
"""

import argparse
import csv
import glob
import sys

POLICIES = ["ff", "b16", "b8", "b4", "b2", "b1"]
COLUMNS = [
    "firstfit",
    "folding",
    "rfold_8x8x8",
    "rfold_4x4x4",
    "rfold_2x2x2",
    "rfold_1x1x1",
]


def rate_tag(rate: str) -> str:
    """Mirror of rate_tag() in run_placeability_census.sh."""
    return "idle" if float(rate) == 0 else rate.lstrip("0").lstrip(".")


def rate_label(rate: str) -> str:
    if float(rate) == 0:
        return ""
    pct = float(rate) * 100
    return f"_fail{pct:g}pct"


def load(pattern):
    out = {}
    for fn in glob.glob(pattern):
        for line in open(fn):
            p = line.split()
            if len(p) >= 4 and p[3] in ("PLACED", "DROP", "DEFER", "NOPE"):
                out[(int(p[0]), int(p[1]), int(p[2]))] = p[3]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--raw", required=True, help="dir with probe output")
    ap.add_argument("--shapes", required=True, help="shape universe file")
    ap.add_argument(
        "--rates", required=True, help="space-separated failure rates, e.g. '0 0.001'"
    )
    ap.add_argument("--outdir", required=True, help="where CSVs are written")
    args = ap.parse_args()

    shapes = [tuple(map(int, line.split())) for line in open(args.shapes)]
    incomplete = False
    for rate in args.rates.split():
        tag = rate_tag(rate)
        data = {}
        for pol in POLICIES:
            d = load(f"{args.raw}/{pol}_{tag}.*.txt")
            missing = sum(1 for s in shapes if s not in d)
            if missing:
                print(
                    f"!! {pol} rate={rate}: {missing} shapes missing; "
                    f"skipping this rate",
                    file=sys.stderr,
                )
                incomplete = True
                break
            data[pol] = d
        if len(data) != len(POLICIES):
            continue
        path = f"{args.outdir}/placeability_16x16x16{rate_label(rate)}.csv"
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["a", "b", "c", "dim"] + COLUMNS)
            for s in shapes:
                nd = sum(1 for x in s if x > 1)
                dim = {0: "1D", 1: "1D", 2: "2D", 3: "3D"}[nd]
                w.writerow(
                    [s[0], s[1], s[2], dim]
                    + ["y" if data[p][s] == "PLACED" else "n" for p in POLICIES]
                )
        counts = ", ".join(
            f"{c}={sum(1 for s in shapes if data[p][s] == 'PLACED')}"
            for c, p in zip(COLUMNS, POLICIES)
        )
        print(f"rate {rate}: {path}\n  placed: {counts}")
    return 1 if incomplete else 0


if __name__ == "__main__":
    sys.exit(main())
