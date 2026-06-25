#!/usr/bin/env python3
"""OK/FAIL lib-build log -> calibration/palette.json (spec section 4.2).

palette.json records, per model, which candidate orderings stage actually
generated (pass) and which failed (with reason). Hard gate: every grid size
(plus the 1-node exception) must keep >=1 passing shape in BOTH models —
otherwise exit 1 listing the dead sizes, so the palette never shrinks
silently.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from palette import ATTN_HEADS, NUM_STACKS, grid  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--log", required=True, help="lib build OK/FAIL log")
    ap.add_argument("--out", required=True, help="palette.json path")
    ap.add_argument(
        "--lat-kvhead",
        type=int,
        required=True,
        help="kvhead the lat model was built with (probe result)",
    )
    args = ap.parse_args()

    passed = {"lat": [], "bw": []}
    failed = {"lat": {}, "bw": {}}
    with open(args.log) as f:
        for line in f:
            parts = line.strip().split(None, 2)
            if len(parts) < 3 or parts[0] not in ("OK", "FAIL"):
                continue
            tag, model, rest = parts
            if model not in passed:
                continue
            if tag == "OK":
                passed[model].append(rest)
            else:
                shape, _, reason = rest.partition(":")
                failed[model][shape.strip()] = reason.strip()

    sizes = [1] + grid("small") + grid("large")
    dead = []
    for model in ("lat", "bw"):
        have = {tuple(int(d) for d in sh.split("x")) for sh in passed[model]}
        for s in sizes:
            if not any(a * b * c == s for a, b, c in have):
                dead.append(f"{model}:{s}")
    if dead:
        sys.exit(
            "sizes with zero passing shapes (spec 4.2 hard gate): " + " ".join(dead)
        )

    with open(args.out, "w") as f:
        json.dump(
            {
                "attn_heads": ATTN_HEADS,
                "num_stacks": NUM_STACKS,
                "lat_kvhead": args.lat_kvhead,
                "pass": {m: sorted(v) for m, v in passed.items()},
                "fail": failed,
            },
            f,
            indent=1,
            sort_keys=True,
        )
    npass = {m: len(v) for m, v in passed.items()}
    nfail = {m: len(v) for m, v in failed.items()}
    print(f"wrote {args.out}: pass={npass} fail={nfail}")


if __name__ == "__main__":
    main()
