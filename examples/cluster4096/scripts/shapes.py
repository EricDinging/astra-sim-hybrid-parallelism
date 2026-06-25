"""Legal job-shape library for the cluster4096 (16x16x16 torus) trace generator.

WHY THE LEGAL SET IS PURELY GEOMETRIC
-------------------------------------
A job shape is an ordered triple A x B x C, mapped to stage parallelism dims as
A=dp, B=tp, C=pp. Each dimension is independently 1 or an even integer <= 16,
and the size A*B*C must be <= 4096 (the 16x16x16 torus capacity; automatically
satisfied since each dim <= 16).

Empirically verified on 2026-06-25 by running stage (main.py inside the
astra:latest image) over the bw model (head=32, kvhead=8): stage accepts EVERY
such shape with NO divisibility constraint on tp. tp in {6, 10, 12, 14} (which
do NOT divide head=32) and tp=16 with kvhead=8 all produce valid Chakra traces;
dp is unconstrained and pp only needs pp <= num_stacks (=16), auto-satisfied.

This CONTRADICTS the `tp | ATTN_HEADS` assumption baked into the older
examples/traces-for-16x16x16/scripts/palette.py. That rule is a *fidelity*
choice (a clean integer head-per-rank split), NOT a stage requirement. Do NOT
re-introduce head-divisibility pruning here: the legal set is defined by
geometry alone, and this module is its single authority.
"""

from __future__ import annotations

DIM_DOMAIN: tuple[int, ...] = (1, 2, 4, 6, 8, 10, 12, 14, 16)
MAX_SIZE: int = 4096  # 16 * 16 * 16 torus capacity
MODELS: tuple[str, ...] = ("bw",)


def _check_model(model: str) -> None:
    if model not in MODELS:
        raise ValueError(f"unknown model {model!r}; known models: {MODELS}")


def is_legal(model: str, a: int, b: int, c: int) -> bool:
    """True iff A x B x C is a legal job shape for the given model."""
    _check_model(model)
    return (
        a in DIM_DOMAIN
        and b in DIM_DOMAIN
        and c in DIM_DOMAIN
        and a * b * c <= MAX_SIZE
    )


def all_legal_shapes(model: str) -> list[tuple[int, int, int]]:
    """All legal ordered shapes, sorted by (size, shape) for reproducibility."""
    _check_model(model)
    shapes = [
        (a, b, c)
        for a in DIM_DOMAIN
        for b in DIM_DOMAIN
        for c in DIM_DOMAIN
        if is_legal(model, a, b, c)
    ]
    shapes.sort(key=lambda t: (t[0] * t[1] * t[2], t))
    return shapes


def shapes_for_size(model: str, size: int) -> list[tuple[int, int, int]]:
    """Legal shapes whose product equals `size` (used by the future sampler)."""
    return [s for s in all_legal_shapes(model) if s[0] * s[1] * s[2] == size]


def fmt_shape(shape: tuple[int, int, int]) -> str:
    """(2, 4, 8) -> '2x4x8'."""
    return "x".join(str(d) for d in shape)


def parse_shape(text: str) -> tuple[int, int, int]:
    """'2x4x8' -> (2, 4, 8)."""
    parts = text.split("x")
    if len(parts) != 3:
        raise ValueError(f"expected 'AxBxC', got {text!r}")
    a, b, c = (int(p) for p in parts)
    return (a, b, c)


def main(argv: list[str] | None = None) -> int:
    import argparse

    ap = argparse.ArgumentParser(description="cluster4096 legal-shape utility")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_list = sub.add_parser("list", help="print all legal shapes for a model")
    p_list.add_argument("model", choices=MODELS)
    args = ap.parse_args(argv)

    if args.cmd == "list":
        for shape in all_legal_shapes(args.model):
            print(fmt_shape(shape))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
