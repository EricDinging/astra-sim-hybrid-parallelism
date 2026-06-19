#!/usr/bin/env python3
"""build_palette_json.py: records pass/fail, gates per-size coverage."""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from palette import grid, shapes_for  # noqa: E402

BPJ = os.path.join(HERE, "build_palette_json.py")


def make_log(path, drop_size=None):
    with open(path, "w") as f:
        for model in ("lat", "bw"):
            for size in [1] + grid("small") + grid("large"):
                for sh in shapes_for(size):
                    if size == drop_size:
                        f.write(f"FAIL {model} {sh}: probe says no\n")
                    else:
                        f.write(f"OK {model} {sh}\n")


def run(log, out):
    return subprocess.run(
        [sys.executable, BPJ, "--log", log, "--out", out, "--lat-kvhead", "4"],
        capture_output=True,
        text=True,
    )


def main():
    tmp = tempfile.mkdtemp(prefix="test_bpj_")
    log = os.path.join(tmp, "build.log")
    out = os.path.join(tmp, "palette.json")
    make_log(log)
    r = run(log, out)
    assert r.returncode == 0, r.stderr
    pj = json.load(open(out))
    assert pj["lat_kvhead"] == 4
    assert "1x1x1" in pj["pass"]["lat"]
    assert len(pj["pass"]["lat"]) == 1 + 113 + 84
    # a fully-failed size must be a hard error naming the size
    make_log(log, drop_size=24)
    r = run(log, out)
    assert r.returncode != 0 and "24" in r.stderr, r.stderr
    print("PASS build_palette_json.py")


if __name__ == "__main__":
    main()
