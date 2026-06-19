#!/usr/bin/env python3
"""gen_mixed.py: pmf realization, 1-node slice, determinism, ia-linearity."""

import csv
import os
import shutil
import subprocess
import sys
import tempfile
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from palette import grid, shapes_for, weights  # noqa: E402

GEN = os.path.join(HERE, "gen_mixed.py")


def make_lib(root, models=("lat", "bw")):
    """Empty chakra_trace.0.et markers are a valid lib for gen_mixed."""
    for model in models:
        for size in [1] + grid("small"):
            for sh in shapes_for(size):
                d = os.path.join(root, model, sh)
                os.makedirs(d, exist_ok=True)
                open(os.path.join(d, "chakra_trace.0.et"), "w").close()


def gen(lib, out, n, ia, seed=1, onf=0.05, bw_frac=0.0):
    subprocess.run(
        [
            sys.executable,
            GEN,
            "--out",
            out,
            "--lib",
            lib,
            "--n",
            str(n),
            "--seed",
            str(seed),
            "--bw-frac",
            str(bw_frac),
            "--sizes",
            ",".join(str(s) for s in grid("small")),
            "--one-node-frac",
            str(onf),
            "--mean-ia-ns",
            str(ia),
        ],
        check=True,
        capture_output=True,
        text=True,
    )


def read_col(path, col, cast=int):
    with open(path, newline="") as f:
        return [cast(r[col]) for r in csv.DictReader(f)]


def main():
    tmp = tempfile.mkdtemp(prefix="test_gm_")
    try:
        lib = os.path.join(tmp, "lib")
        make_lib(lib)
        a, b, c = (os.path.join(tmp, x) for x in "abc")
        n = 4000
        gen(lib, a, n, 1e6)
        sizes = read_col(os.path.join(a, "arrivals.csv"), "num_ranks")
        cnt = Counter(sizes)
        one_frac = cnt[1] / n
        assert 0.03 <= one_frac <= 0.07, f"1-node frac {one_frac}"
        pmf = dict(weights("small"))
        for s in (8, 16, 24):  # head sizes: enough mass for a tight check
            exp = 0.95 * pmf[s]
            got = cnt[s] / n
            sig = (exp * (1 - exp) / n) ** 0.5
            assert abs(got - exp) <= 4 * sig, f"size {s}: {got} vs {exp}"
        # determinism: same args -> byte-identical arrivals
        gen(lib, b, n, 1e6)
        for fn in ("arrivals.csv", "job_models.csv"):
            assert (
                open(os.path.join(a, fn)).read() == open(os.path.join(b, fn)).read()
            ), fn
        # ia-linearity: job sequence identical, span scales ~2x
        gen(lib, c, n, 2e6)
        assert (
            open(os.path.join(a, "job_models.csv")).read()
            == open(os.path.join(c, "job_models.csv")).read()
        )
        sa = read_col(os.path.join(a, "arrivals.csv"), "arrival_time_ns")
        sc = read_col(os.path.join(c, "arrivals.csv"), "arrival_time_ns")
        ratio = (max(sc) - min(sc)) / (max(sa) - min(sa))
        assert abs(ratio - 2.0) < 1e-3, f"span ratio {ratio}"
        # hard gate: missing size in the needed model lib
        lib2 = os.path.join(tmp, "lib2")
        make_lib(lib2, models=("lat",))
        shutil.rmtree(os.path.join(lib2, "lat", "2x2x2"))  # size 8 gone
        r = subprocess.run(
            [
                sys.executable,
                GEN,
                "--out",
                os.path.join(tmp, "d"),
                "--lib",
                lib2,
                "--n",
                "10",
                "--seed",
                "1",
                "--bw-frac",
                "0",
                "--sizes",
                ",".join(str(s) for s in grid("small")),
                "--one-node-frac",
                "0.05",
                "--mean-ia-ns",
                "1e6",
            ],
            capture_output=True,
            text=True,
        )
        assert r.returncode != 0 and "missing" in r.stderr, r.stderr
        print("PASS gen_mixed.py")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
