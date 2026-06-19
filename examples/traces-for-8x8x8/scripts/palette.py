#!/usr/bin/env python3
"""Enumerate every ordered AxBxC shape (each dim <= max-dim) for given sizes.

The sweep samples a job's shape uniformly from this set (spec: uniform over
all factorizations), and the STG library is built to contain exactly this
set, so gen_mixed.py's uniform-over-library choice realizes the spec.
"""

import argparse


def shapes_for(size, max_dim=8):
    out = []
    for a in range(1, max_dim + 1):
        if size % a:
            continue
        ab = size // a
        for b in range(1, max_dim + 1):
            if ab % b:
                continue
            c = ab // b
            if c <= max_dim:
                out.append(f"{a}x{b}x{c}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sizes", nargs="+", type=int)
    ap.add_argument("--max-dim", type=int, default=8)
    ap.add_argument("--counts", action="store_true", help="print per-size counts")
    args = ap.parse_args()
    if args.counts:
        total = 0
        for s in args.sizes:
            n = len(shapes_for(s, args.max_dim))
            total += n
            print(f"{s}:{n}")
        print(f"total:{total}")
    else:
        for s in args.sizes:
            for sh in shapes_for(s, args.max_dim):
                print(sh)


if __name__ == "__main__":
    main()
