#!/usr/bin/env python3
"""palette.py invariants pinned to the spec's enumeration (spec 3.1/3.2)."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from palette import PMF_EXP, grid, shapes_for, weights  # noqa: E402

EXPECTED_SMALL = [
    8,
    16,
    24,
    32,
    40,
    48,
    56,
    64,
    72,
    80,
    96,
    112,
    120,
    128,
    144,
    160,
    168,
    192,
    200,
    224,
    240,
    256,
]
EXPECTED_LARGE = [
    512,
    560,
    576,
    640,
    672,
    768,
    784,
    800,
    896,
    960,
    1024,
    1120,
    1152,
    1280,
    1344,
    1536,
    1568,
    1600,
    1792,
    1920,
    2048,
]


def main():
    assert grid("small") == EXPECTED_SMALL, grid("small")
    assert grid("large") == EXPECTED_LARGE, grid("large")
    n_small = sum(len(shapes_for(s)) for s in EXPECTED_SMALL)
    n_large = sum(len(shapes_for(s)) for s in EXPECTED_LARGE)
    assert n_small == 113, n_small  # candidate orderings, spec 4.2
    assert n_large == 84, n_large
    assert shapes_for(1) == ["1x1x1"]
    assert 216 not in grid("small")  # 6x6x6 only: tp=6 does not divide 16
    for s in EXPECTED_SMALL + EXPECTED_LARGE:
        for sh in shapes_for(s):
            dp, tp, pp = (int(v) for v in sh.split("x"))
            assert dp * tp * pp == s
            assert all(d % 2 == 0 and 2 <= d <= 16 for d in (dp, tp, pp)), sh
            assert 16 % tp == 0, sh  # tp | ATTN_HEADS
            assert pp <= 16, sh  # pp <= NUM_STACKS
    for group in ("small", "large"):
        w = weights(group)
        assert abs(sum(p for _, p in w) - 1.0) < 1e-9
        ps = [p for _, p in w]
        assert ps == sorted(ps, reverse=True)  # pmf ~ s^-1.5 is monotone
    assert PMF_EXP == 1.5
    print("PASS palette.py")


if __name__ == "__main__":
    main()
