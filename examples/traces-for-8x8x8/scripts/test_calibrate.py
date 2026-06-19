#!/usr/bin/env python3
"""End-to-end test of calibrate.py's exact-ia math on a synthetic library.

gen_mixed.py only stats chakra_trace.0.et, so empty marker files make a
valid fake mixlib. We invent service times, ask `calibrate.py ia` for the
exact mean-ia at rho=0.15, regenerate with it, and assert the realized
isolated offered load is on target (spec section 6 exactness claim).
"""

import csv
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from calibrate import CAPACITY, arrival_span, job_work, read_service  # noqa: E402
from palette import shapes_for  # noqa: E402

SIZES = (4, 8, 16)
WEIGHTS = "4:0.25,8:0.125,16:0.0625"


def make_lib(root):
    for model in ("lat", "bw"):
        for size in SIZES:
            for sh in shapes_for(size):
                d = os.path.join(root, model, sh)
                os.makedirs(d)
                open(os.path.join(d, "chakra_trace.0.et"), "w").close()


def main():
    tmp = tempfile.mkdtemp(prefix="test_calib_")
    try:
        lib = os.path.join(tmp, "lib")
        make_lib(lib)
        svc_csv = os.path.join(tmp, "svc.csv")
        with open(svc_csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["model", "shape", "size", "svc_ns"])
            for model in ("lat", "bw"):
                for size in SIZES:
                    for i, sh in enumerate(shapes_for(size)):
                        w.writerow(
                            [model, sh, size, 1_000_000 + 137_000 * i + size * 31_000]
                        )
        rho = 0.15
        out = subprocess.run(
            [
                sys.executable,
                os.path.join(HERE, "calibrate.py"),
                "ia",
                "--lib",
                lib,
                "--svc",
                svc_csv,
                "--bw-frac",
                "0",
                "--size-weights",
                WEIGHTS,
                "--rho",
                str(rho),
                "--n",
                "400",
                "--seed",
                "1",
            ],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.split()
        ia = float(out[0])
        cell = os.path.join(tmp, "cell")
        subprocess.run(
            [
                sys.executable,
                os.path.join(HERE, "gen_mixed.py"),
                "--out",
                cell,
                "--lib",
                lib,
                "--n",
                "400",
                "--seed",
                "1",
                "--bw-frac",
                "0",
                "--size-weights",
                WEIGHTS,
                "--mean-ia-ns",
                str(ia),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        realized = job_work(cell, read_service(svc_csv)) / (
            CAPACITY * arrival_span(cell)
        )
        err = abs(realized - rho)
        assert err < 0.001, (
            f"realized rho {realized:.5f} vs target {rho} (err {err:.5f})"
        )
        print(f"PASS realized rho={realized:.5f} target={rho} ia={ia:.1f}ns")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
