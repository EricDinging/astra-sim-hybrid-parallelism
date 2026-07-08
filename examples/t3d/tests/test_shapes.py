"""Self-running unit tests for shapes.py (run: python3 tests/test_shapes.py)."""

import os
import sys

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts")
)

import shapes  # noqa: E402

shapes.init((16, 16, 16))


def test_dim_domain_is_one_or_even_le_16():
    assert shapes.DIM_DOMAIN == (1, 2, 4, 6, 8, 10, 12, 14, 16)
    for d in shapes.DIM_DOMAIN:
        assert d == 1 or (d % 2 == 0 and d <= 16)


def test_init_derives_domain_from_cluster_dims():
    try:
        shapes.init((8, 8, 8))
        assert shapes.DIM_DOMAIN == (1, 2, 4, 6, 8)
        assert shapes.MAX_SIZE == 512
        # 5 x 5 x 5 ordered triples, all <= 512.
        assert len(shapes.all_legal_shapes("bw")) == 125
    finally:
        shapes.init((16, 16, 16))


def test_uninitialized_raises():
    domain, size = shapes.DIM_DOMAIN, shapes.MAX_SIZE
    try:
        shapes.DIM_DOMAIN, shapes.MAX_SIZE = (), 0
        try:
            shapes.is_legal("bw", 2, 2, 2)
            raise AssertionError("expected RuntimeError")
        except RuntimeError:
            pass
    finally:
        shapes.DIM_DOMAIN, shapes.MAX_SIZE = domain, size


def test_candidate_count_is_729():
    # 9 x 9 x 9 ordered triples, all <= 4096, none pruned.
    assert len(shapes.all_legal_shapes("bw")) == 729


def test_every_shape_is_legal_and_in_domain():
    for a, b, c in shapes.all_legal_shapes("bw"):
        assert a in shapes.DIM_DOMAIN
        assert b in shapes.DIM_DOMAIN
        assert c in shapes.DIM_DOMAIN
        assert a * b * c <= shapes.MAX_SIZE
        assert shapes.is_legal("bw", a, b, c)


def test_shapes_are_ordered_distinct():
    legal = set(shapes.all_legal_shapes("bw"))
    assert (1, 2, 4) in legal
    assert (4, 2, 1) in legal
    assert (1, 2, 4) != (4, 2, 1)


def test_no_head_divisibility_pruning():
    # The whole point: tp values that do NOT divide head=32 are still legal.
    for tp in (6, 10, 12, 14):
        assert shapes.is_legal("bw", 1, tp, 1)
    # tp=16 (with kvhead=8 at gen time) is legal too.
    assert shapes.is_legal("bw", 1, 16, 1)


def test_illegal_dims_rejected():
    assert not shapes.is_legal("bw", 3, 2, 2)  # 3 not in domain
    assert not shapes.is_legal("bw", 2, 2, 18)  # 18 > 16, not in domain


def test_sorted_by_size_then_shape():
    legal = shapes.all_legal_shapes("bw")
    keys = [(a * b * c, (a, b, c)) for a, b, c in legal]
    assert keys == sorted(keys)
    assert legal[0] == (1, 1, 1)


def test_shapes_for_size_is_subset_with_right_product():
    all_legal = set(shapes.all_legal_shapes("bw"))
    for size in (1, 8, 64, 4096):
        for shape in shapes.shapes_for_size("bw", size):
            assert shape in all_legal
            a, b, c = shape
            assert a * b * c == size
    assert shapes.shapes_for_size("bw", 4096) == [(16, 16, 16)]


def test_fmt_and_parse_roundtrip():
    assert shapes.fmt_shape((2, 4, 8)) == "2x4x8"
    assert shapes.parse_shape("2x4x8") == (2, 4, 8)


def test_unknown_model_raises():
    try:
        shapes.is_legal("lat", 1, 1, 1)
    except ValueError:
        return
    assert False, "expected ValueError for unknown model"


if __name__ == "__main__":
    fns = [
        v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)
    ]
    failed = 0
    for fn in fns:
        try:
            fn()
            print(f"PASS {fn.__name__}")
        except Exception as exc:  # noqa: BLE001
            failed += 1
            print(f"FAIL {fn.__name__}: {exc}")
    print(f"{len(fns) - failed}/{len(fns)} passed")
    sys.exit(1 if failed else 0)
