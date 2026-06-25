"""Self-running unit tests for gen_traces.py pure helpers (no docker needed)."""

import os
import tempfile

import gen_traces


def _touch(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write("")


def test_build_worklist_text_format():
    text = gen_traces.build_worklist_text([(1, 2, 4), (16, 16, 16)])
    assert text == "1 2 4\n16 16 16\n"


def test_et0_path():
    p = gen_traces.et0_path("/out", "bw", (2, 4, 8))
    assert p == os.path.join("/out", "bw", "2x4x8", "chakra_trace.0.et")


def test_shapes_to_build_filters_existing():
    with tempfile.TemporaryDirectory() as d:
        candidates = [(2, 2, 2), (4, 4, 4)]
        _touch(gen_traces.et0_path(d, "bw", (2, 2, 2)))  # already built
        todo = gen_traces.shapes_to_build(d, "bw", candidates)
        assert todo == [(4, 4, 4)]


def test_shapes_to_build_all_missing():
    with tempfile.TemporaryDirectory() as d:
        candidates = [(2, 2, 2), (4, 4, 4)]
        assert gen_traces.shapes_to_build(d, "bw", candidates) == candidates


def test_select_shapes_smoke():
    assert gen_traces.select_shapes("bw", None, True) == [(2, 2, 2)]


def test_select_shapes_only():
    got = gen_traces.select_shapes("bw", "2x4x8,16x16x16", False)
    assert got == [(2, 4, 8), (16, 16, 16)]


def test_select_shapes_full_is_729():
    assert len(gen_traces.select_shapes("bw", None, False)) == 729


def test_bw_params_exact():
    # BW_PARAMS is a spec-frozen constant: assert it byte-for-byte so a stray
    # edit (or a hallucinated rewrite) cannot pass. Must NOT contain dp/tp/pp.
    assert gen_traces.BW_PARAMS == (
        "--model_type dense "
        "--dmodel 8192 --dff 16384 --batch 16 --seq 2048 --dvocal 8192 "
        "--head 32 --kvhead 8 --num_stacks 16 --weight_sharded 0 "
        "--chakra_schema_version v0.0.4"
    )
    for flag in ("--dp", "--tp", "--pp"):
        assert flag not in gen_traces.BW_PARAMS


def test_select_shapes_only_rejects_illegal():
    for bad in ("3x5x7", "32x1x1", "1x1x18"):
        try:
            gen_traces.select_shapes("bw", bad, False)
        except ValueError:
            continue
        raise AssertionError(f"expected ValueError for illegal --only {bad}")


if __name__ == "__main__":
    import sys

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
