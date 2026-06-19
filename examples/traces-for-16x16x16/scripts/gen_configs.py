#!/usr/bin/env python3
"""Generate configs/ for the 16x16x16 sweep (spec section 5.1).

Writes bandwidth_schedule.txt / latency_schedule.txt (4096x4096, 6-neighbor
3-D torus, BW 50 GB/s, LT 1000 ns) and network.yml (npus_count 4096).
system.json / remote_memory.json are static committed copies, not touched.

Id encoding matches precomputeRoutes_DOR: id = z*Dy*Dx + y*Dx + x (x fastest).
All non-neighbor cells including the diagonal are 0 (no loopback link).

--check re-reads the generated files and asserts the invariants instead of
writing (frame lines, row/col counts, 6 neighbors per row at the right ids,
symmetry, zero diagonal). The smoke phase runs gen + check end-to-end.
"""

import argparse
import os
import sys

DX = DY = DZ = 16
N = DX * DY * DZ
BW_GBPS = 50
LT_NS = 1000

NETWORK_YML = """topology: [ FullyConnected ]
npus_count: [ {n} ]
bandwidth: [ 50.0 ]  # GB/s
latency: [ 1000.0 ]  # ns
reconfig_time: [ 1000.0 ]  # ns
"""


def neighbors(i):
    z, rem = divmod(i, DY * DX)
    y, x = divmod(rem, DX)
    for dx, dy, dz in (
        (1, 0, 0),
        (-1, 0, 0),
        (0, 1, 0),
        (0, -1, 0),
        (0, 0, 1),
        (0, 0, -1),
    ):
        yield (((z + dz) % DZ) * DY * DX + ((y + dy) % DY) * DX + (x + dx) % DX)


def write_matrix(path, tag, val):
    with open(path, "w") as f:
        f.write(f"{tag} 0\n")
        sval = str(val)
        for i in range(N):
            row = ["0"] * N
            for j in neighbors(i):
                row[j] = sval
            f.write(" ".join(row) + "\n")
        f.write("END\n")
    print(f"wrote {path}")


def check_matrix(path, tag, val):
    with open(path) as f:
        lines = f.read().splitlines()
    assert lines, f"{path}: empty file"
    assert lines[0] == f"{tag} 0", f"{path}: bad header {lines[0]!r}"
    assert lines[-1] == "END", f"{path}: missing END"
    assert len(lines) == N + 2, f"{path}: {len(lines) - 2} rows, want {N}"
    rows = []
    for i, line in enumerate(lines[1:-1]):
        vals = [int(v) for v in line.split()]
        assert len(vals) == N, f"{path} row {i}: {len(vals)} cols"
        nz = {j for j, v in enumerate(vals) if v}
        assert vals[i] == 0, f"{path} row {i}: nonzero diagonal"
        assert nz == set(neighbors(i)), f"{path} row {i}: wrong neighbor set"
        assert all(vals[j] == val for j in nz), f"{path} row {i}: bad value"
        rows.append(nz)
    for i in range(N):  # symmetry: j in nz(i) <=> i in nz(j)
        for j in rows[i]:
            assert i in rows[j], f"{path}: asymmetric link {i}->{j}"
    print(f"OK {path}: {N}x{N}, 6 neighbors/row, symmetric, diag 0")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--out",
        default=os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "configs"
        ),
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="verify existing files instead of writing",
    )
    args = ap.parse_args()
    bw = os.path.join(args.out, "bandwidth_schedule.txt")
    lt = os.path.join(args.out, "latency_schedule.txt")
    yml = os.path.join(args.out, "network.yml")
    if args.check:
        check_matrix(bw, "BW", BW_GBPS)
        check_matrix(lt, "LT", LT_NS)
        with open(yml) as f:
            assert f"npus_count: [ {N} ]" in f.read(), "network.yml wrong N"
        print(f"OK {yml}")
        return
    os.makedirs(args.out, exist_ok=True)
    write_matrix(bw, "BW", BW_GBPS)
    write_matrix(lt, "LT", LT_NS)
    with open(yml, "w") as f:
        f.write(NETWORK_YML.format(n=N))
    print(f"wrote {yml}")


if __name__ == "__main__":
    sys.exit(main())
