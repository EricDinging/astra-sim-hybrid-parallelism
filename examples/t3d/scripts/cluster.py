"""Cluster geometry config: the torus dims live in <example root>/cluster.json,
written once by `./reproduce.py prereq`. Everything geometric -- the legal job
shapes, cluster capacity, --npus-per-dim, the experiment max-job sizes, and
the launcher's memory envelope -- derives from it at runtime; nothing else in
the harness hardcodes the cluster size."""

from __future__ import annotations

import json
import os

FILENAME = "cluster.json"


def default_root() -> str:
    """The example root (parent of this scripts/ dir); same relative layout
    locally and on a deployed worker workspace."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def path(root: str) -> str:
    return os.path.join(root, FILENAME)


def parse_dims(text: str) -> tuple[int, int, int]:
    """'16x16x16' (or '16,16,16') -> (16, 16, 16)."""
    parts = text.strip().lower().replace(",", "x").split("x")
    if len(parts) != 3:
        raise ValueError(f"expected AxBxC, got {text!r}")
    a, b, c = (int(p) for p in parts)
    if min(a, b, c) < 1:
        raise ValueError(f"axes must be positive, got {text!r}")
    return (a, b, c)


def fmt(dims: tuple[int, int, int]) -> str:
    """(16, 16, 16) -> '16x16x16'."""
    return "x".join(str(d) for d in dims)


def save(root: str, dims: tuple[int, int, int]) -> None:
    with open(path(root), "w") as f:
        json.dump({"dims": list(dims)}, f)
        f.write("\n")


def load(root: str) -> tuple[int, int, int]:
    """The cluster dims, or SystemExit when the config has not been written
    yet (locally that means `./reproduce.py prereq` hasn't run)."""
    try:
        with open(path(root)) as f:
            dims = parse_dims(fmt(json.load(f)["dims"]))
    except (OSError, KeyError, TypeError, ValueError) as e:
        raise SystemExit(
            f"{path(root)} missing or invalid ({e}) -- "
            f"set the cluster dims with ./reproduce.py prereq first"
        ) from e
    return dims


def capacity(dims: tuple[int, int, int]) -> int:
    return dims[0] * dims[1] * dims[2]


def npus_arg(dims: tuple[int, int, int]) -> str:
    """(16, 16, 16) -> '16,16,16' (the binary's --npus-per-dim format)."""
    return ",".join(str(d) for d in dims)
