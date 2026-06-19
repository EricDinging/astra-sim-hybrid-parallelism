#!/usr/bin/env python3
"""End-to-end test of calibrate.py's exact-ia math on a synthetic library,
ported to the 16x16x16 interface (--sizes/--pmf-exp/--one-node-frac).
Also covers the new `merge` subcommand."""

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

SIZES = (8, 16, 32)
SIZES_ARG = ",".join(str(s) for s in SIZES)


def make_lib(root):
    for model in ("lat", "bw"):
        for size in (1,) + SIZES:
            for sh in shapes_for(size):
                d = os.path.join(root, model, sh)
                os.makedirs(d)
                open(os.path.join(d, "chakra_trace.0.et"), "w").close()


def write_svc(path, root):
    rows = []
    for model in ("lat", "bw"):
        for size in (1,) + SIZES:
            for i, sh in enumerate(shapes_for(size)):
                rows.append([model, sh, size, 1_000_000 + 137_000 * i + size * 31_000])
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["model", "shape", "size", "svc_ns"])
        w.writerows(rows)
    return rows


def main():
    tmp = tempfile.mkdtemp(prefix="test_calib_")
    try:
        lib = os.path.join(tmp, "lib")
        make_lib(lib)
        svc_csv = os.path.join(tmp, "svc.csv")
        rows = write_svc(svc_csv, lib)
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
                "--sizes",
                SIZES_ARG,
                "--one-node-frac",
                "0.05",
                "--rho",
                str(rho),
                "--n",
                "120",
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
                str(ia),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        realized = job_work(cell, read_service(svc_csv)) / (
            CAPACITY * arrival_span(cell)
        )
        assert CAPACITY == 4096, CAPACITY
        err = abs(realized - rho)
        assert err < 0.001, f"realized rho {realized:.5f} (err {err:.5f})"
        # merge: one shard per entry -> identical service_times.csv
        shards = os.path.join(tmp, "shards")
        os.makedirs(shards)
        for model, sh, size, svc in rows:
            with open(os.path.join(shards, f"{model}_{sh}.csv"), "w") as f:
                f.write(f"{model},{sh},{size},{svc}\n")
        merged = os.path.join(tmp, "service_times.csv")
        subprocess.run(
            [
                sys.executable,
                os.path.join(HERE, "calibrate.py"),
                "merge",
                "--lib",
                lib,
                "--shards",
                shards,
                "--out",
                merged,
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        assert read_service(merged) == read_service(svc_csv)
        # merge with a missing shard must fail and name the entry
        os.unlink(os.path.join(shards, "lat_2x2x2.csv"))
        r = subprocess.run(
            [
                sys.executable,
                os.path.join(HERE, "calibrate.py"),
                "merge",
                "--lib",
                lib,
                "--shards",
                shards,
                "--out",
                merged,
            ],
            capture_output=True,
            text=True,
        )
        assert r.returncode != 0 and "lat/2x2x2" in r.stderr, r.stderr
        print(f"PASS realized rho={realized:.5f} ia={ia:.1f}ns + merge")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
