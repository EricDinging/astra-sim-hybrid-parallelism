#!/usr/bin/env python3
"""verify_trace.py gates: pass on an on-target cell, fail on a wrong mix,
fail on off-target rho."""

import csv
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_calibrate import SIZES_ARG, make_lib, write_svc  # noqa: E402

VT = os.path.join(HERE, "verify_trace.py")


def run_vt(cell, svc, lib, rho):
    return subprocess.run(
        [
            sys.executable,
            VT,
            cell,
            "--svc",
            svc,
            "--lib",
            lib,
            "--rho",
            str(rho),
            "--sizes",
            SIZES_ARG,
            "--one-node-frac",
            "0.05",
        ],
        capture_output=True,
        text=True,
    )


def main():
    tmp = tempfile.mkdtemp(prefix="test_vt_")
    try:
        lib = os.path.join(tmp, "lib")
        make_lib(lib)
        svc = os.path.join(tmp, "svc.csv")
        write_svc(svc, lib)
        ia = subprocess.run(
            [
                sys.executable,
                os.path.join(HERE, "calibrate.py"),
                "ia",
                "--lib",
                lib,
                "--svc",
                svc,
                "--bw-frac",
                "0",
                "--sizes",
                SIZES_ARG,
                "--one-node-frac",
                "0.05",
                "--rho",
                "0.15",
                "--n",
                "120",
                "--seed",
                "1",
            ],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.split()[0]
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
                "120",
                "--seed",
                "1",
                "--bw-frac",
                "0",
                "--sizes",
                SIZES_ARG,
                "--one-node-frac",
                "0.05",
                "--mean-ia-ns",
                ia,
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        r = run_vt(cell, svc, lib, 0.15)
        assert r.returncode == 0, r.stdout + r.stderr
        assert "OK" in r.stdout
        # off-target rho must fail
        r = run_vt(cell, svc, lib, 0.85)
        assert r.returncode != 0 and "rho_isolated" in r.stdout
        # structurally wrong mix must fail: rewrite all sizes to 32
        arr = os.path.join(cell, "arrivals.csv")
        with open(arr, newline="") as f:
            rows = list(csv.DictReader(f))
        for row in rows:
            row["num_ranks"], row["shape"] = "32", "2x2x8"
        with open(arr, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=rows[0].keys())
            w.writeheader()
            w.writerows(rows)
        r = run_vt(cell, svc, lib, 0.15)
        assert r.returncode != 0 and "size" in r.stdout
        print("PASS verify_trace.py")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
