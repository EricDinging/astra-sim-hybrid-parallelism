"""Generate torus or fullmesh BW/LT schedule matrices for the reconfigurable
frontend.

Emits ``bandwidth_schedule.txt`` and ``latency_schedule.txt`` for a Dx x Dy x Dz
3-D torus (default 8x8x8 = 512 NPUs) or, with ``--topo fullmesh``, a fully
connected mesh over the same N nodes. Each file is a single NxN block framed by
``BW 0`` / ``LT 0`` ... ``END``; entry [i][j] is the *direct link* i -> j.

NPU id encoding matches precomputeRoutes_DOR in the reconfigurable
TopologyManager: ``id = z*Dy*Dx + y*Dx + x`` (x fastest-varying). Each node has
exactly 6 neighbors -- (x +/- 1 mod Dx, y, z), (x, y +/- 1 mod Dy, z),
(x, y, z +/- 1 mod Dz) -- set to the per-link BW / LT; every other cell
including the diagonal is 0 (a node has no loopback link). The matrix is
symmetric. Under DOR (``--npus-per-dim`` passed) these 6 neighbor cells are the
only ones consulted, at chunk send time.

``--topo fullmesh`` instead sets every off-diagonal cell to the per-link
BW / LT, as required by the binary's ``--fullmesh`` startup validation (direct
1-hop routing); dims then only determine N.
"""

from __future__ import annotations

import argparse
import os


def neighbor_ids(idx: int, dx: int, dy: int, dz: int) -> list[int]:
    """Return the 6 torus-neighbor ids of NPU ``idx``."""
    x = idx % dx
    y = (idx // dx) % dy
    z = idx // (dx * dy)
    out = []
    for nx in ((x + 1) % dx, (x - 1) % dx):
        out.append(z * dy * dx + y * dx + nx)
    for ny in ((y + 1) % dy, (y - 1) % dy):
        out.append(z * dy * dx + ny * dx + x)
    for nz in ((z + 1) % dz, (z - 1) % dz):
        out.append(nz * dy * dx + y * dx + x)
    return out


def write_matrix(
    path: str,
    tag: str,
    n: int,
    dx: int,
    dy: int,
    dz: int,
    value: int,
    topo: str = "torus",
) -> None:
    """Write one NxN block: ``<tag> 0`` ... N rows ... ``END``."""
    with open(path, "w") as fh:
        fh.write(f"{tag} 0\n")
        for i in range(n):
            if topo == "fullmesh":
                row = [value] * n
                row[i] = 0
            else:
                row = [0] * n
                for j in set(neighbor_ids(i, dx, dy, dz)):
                    row[j] = value
            fh.write(" ".join(map(str, row)) + "\n")
        fh.write("END\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="t3d BW/LT matrix generator")
    ap.add_argument("--dims", default="8,8,8", help="Dx,Dy,Dz (default 8,8,8)")
    ap.add_argument("--bw", type=int, default=50, help="per-link bandwidth GB/s")
    ap.add_argument("--lt", type=int, default=1000, help="per-link latency ns")
    ap.add_argument(
        "--topo",
        default="torus",
        choices=("torus", "fullmesh"),
        help="torus: 6 neighbor cells/row; fullmesh: every off-diagonal cell",
    )
    ap.add_argument("--out", required=True, help="output configs dir")
    args = ap.parse_args(argv)

    dx, dy, dz = (int(v) for v in args.dims.split(","))
    n = dx * dy * dz
    os.makedirs(args.out, exist_ok=True)
    bw_path = os.path.join(args.out, "bandwidth_schedule.txt")
    lt_path = os.path.join(args.out, "latency_schedule.txt")
    write_matrix(bw_path, "BW", n, dx, dy, dz, args.bw, args.topo)
    write_matrix(lt_path, "LT", n, dx, dy, dz, args.lt, args.topo)
    per_row = "N-1 links/row" if args.topo == "fullmesh" else "6 neighbors/row"
    print(
        f"wrote {bw_path} and {lt_path}: {dx}x{dy}x{dz} {args.topo} (N={n}),"
        f" bw={args.bw} lt={args.lt}, {per_row}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
