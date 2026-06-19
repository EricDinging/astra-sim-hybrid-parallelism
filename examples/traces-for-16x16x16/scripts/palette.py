#!/usr/bin/env python3
"""Candidate shape palette for the 16x16x16 sweep (spec sections 3-4).

A size is *eligible* iff it has >=1 ordered factorization dp x tp x pp with
every dim EVEN and <= 16. The single exception shape "1x1x1" carries the
1-node jobs (spec 3.1). Candidate orderings additionally honor the STG-side
constraints (spec 4.1): tp divides ATTN_HEADS (16 — which also divides the
bw model's 32) and pp <= NUM_STACKS (16). The lib build attempts every
candidate and records pass/fail in calibration/palette.json (spec 4.2): this
module enumerates candidates; the build probe is the final arbiter.

Subcommands:
  sizes  {small|large}             eligible size grid, one per line
  shapes {small|large} | <sizes..> candidate orderings, one per line
  weights {small|large}            "<size>:<w>,..." pmf ~ s^-PMF_EXP
"""

import argparse

EVEN_DIMS = (2, 4, 6, 8, 10, 12, 14, 16)
ATTN_HEADS = 16
NUM_STACKS = 16
PMF_EXP = 1.5  # pmf ~ s^-1.5 == truncated Pareto, tail index alpha = 0.5
RANGES = {"small": (2, 256), "large": (512, 2048)}


def shapes_for(size):
    """Candidate ordered (dp, tp, pp) shapes for one size, as 'AxBxC'."""
    if size == 1:
        return ["1x1x1"]
    out = []
    for dp in EVEN_DIMS:
        if size % dp:
            continue
        rest = size // dp
        for tp in EVEN_DIMS:
            if rest % tp or ATTN_HEADS % tp:
                continue
            pp = rest // tp
            if pp in EVEN_DIMS and pp <= NUM_STACKS:
                out.append(f"{dp}x{tp}x{pp}")
    return out


def grid(group):
    lo, hi = RANGES[group]
    return [s for s in range(lo, hi + 1, 2) if shapes_for(s)]


def weights(group):
    g = grid(group)
    tot = sum(s**-PMF_EXP for s in g)
    return [(s, s**-PMF_EXP / tot) for s in g]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("sizes")
    p.add_argument("group", choices=("small", "large"))
    p = sub.add_parser("shapes")
    p.add_argument("what", nargs="+", help="'small'/'large' or explicit sizes")
    p = sub.add_parser("weights")
    p.add_argument("group", choices=("small", "large"))
    args = ap.parse_args()
    if args.cmd == "sizes":
        print("\n".join(str(s) for s in grid(args.group)))
    elif args.cmd == "shapes":
        if args.what[0] in RANGES and len(args.what) == 1:
            sizes = grid(args.what[0])
        else:
            sizes = [int(s) for s in args.what]
        for s in sizes:
            for sh in shapes_for(s):
                print(sh)
    else:
        print(",".join(f"{s}:{w:.10g}" for s, w in weights(args.group)))


if __name__ == "__main__":
    main()
