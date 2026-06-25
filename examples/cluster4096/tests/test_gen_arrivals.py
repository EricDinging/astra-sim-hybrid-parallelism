"""Self-running unit tests for gen_arrivals.py (run: python3 tests/test_gen_arrivals.py)."""

import csv
import os
import random
import sys
import tempfile

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts")
)

import gen_arrivals  # noqa: E402
import shapes  # noqa: E402


def test_legal_sizes_sorted_distinct_and_bounded():
    sizes = gen_arrivals.legal_sizes("bw")
    assert sizes == sorted(set(sizes))
    assert sizes[0] == 1
    assert sizes[-1] == 4096
    # Every size is achievable by some legal shape.
    for s in sizes:
        assert shapes.shapes_for_size("bw", s)


def test_legal_sizes_respects_bounds():
    sizes = gen_arrivals.legal_sizes("bw", size_min=8, size_max=64)
    assert sizes
    assert all(8 <= s <= 64 for s in sizes)


def test_legal_sizes_empty_range_raises():
    try:
        gen_arrivals.legal_sizes("bw", size_min=4097)
    except ValueError:
        return
    assert False, "expected ValueError for empty size range"


def test_sample_size_index_in_range():
    rng = random.Random(0)
    for _ in range(2000):
        k = gen_arrivals.sample_size_index(rng, 100, 0.5)
        assert 0 <= k <= 99


def test_sample_size_index_single_size():
    rng = random.Random(0)
    assert gen_arrivals.sample_size_index(rng, 1, 0.5) == 0


def test_smaller_alpha_has_heavier_tail():
    # Larger alpha concentrates on small indices => smaller mean index.
    rng_a = random.Random(1)
    rng_b = random.Random(1)
    mean_heavy = sum(
        gen_arrivals.sample_size_index(rng_a, 100, 0.5) for _ in range(5000)
    )
    mean_light = sum(
        gen_arrivals.sample_size_index(rng_b, 100, 2.0) for _ in range(5000)
    )
    assert mean_heavy > mean_light


def test_sample_duration_is_one():
    rng = random.Random(0)
    assert all(gen_arrivals.sample_duration(rng) == 1 for _ in range(50))


def test_build_job_sequence_shapes_match_size_and_legal():
    rng = random.Random(7)
    jobs = gen_arrivals.build_job_sequence(rng, 300, "bw", 0.5)
    assert len(jobs) == 300
    for size, shape, iters in jobs:
        assert shapes.is_legal("bw", *shape)
        assert shape[0] * shape[1] * shape[2] == size
        assert iters == 1


def test_build_job_sequence_deterministic():
    a = gen_arrivals.build_job_sequence(random.Random(42), 100, "bw", 0.5)
    b = gen_arrivals.build_job_sequence(random.Random(42), 100, "bw", 0.5)
    assert a == b


def test_load_service_times_roundtrip():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "svc.csv")
        with open(p, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["shape", "size", "svc_per_iter_ns"])
            w.writerow(["2x2x2", 8, 1500])
            w.writerow(["4x1x1", 4, 900])
        table = gen_arrivals.load_service_times(p)
        assert table == {"2x2x2": 1500.0, "4x1x1": 900.0}


def test_make_svc_fn_uniform():
    fn = gen_arrivals.make_svc_fn(None, 123.0)
    assert fn((6, 1, 4)) == 123.0


def test_make_svc_fn_table_missing_shape_raises():
    fn = gen_arrivals.make_svc_fn({"2x2x2": 1500.0}, None)
    assert fn((2, 2, 2)) == 1500.0
    try:
        fn((4, 4, 4))
    except KeyError:
        return
    assert False, "expected KeyError for shape missing from table"


def test_make_svc_fn_requires_one_source():
    try:
        gen_arrivals.make_svc_fn(None, None)
    except ValueError:
        return
    assert False, "expected ValueError when neither table nor uniform given"


def test_compute_work_matches_hand_calc():
    jobs = [(8, (2, 2, 2), 1), (4, (4, 1, 1), 3)]
    fn = gen_arrivals.make_svc_fn(None, 10.0)
    # 8*10*1 + 4*10*3 = 80 + 120 = 200
    assert gen_arrivals.compute_work(jobs, fn) == 200.0


def test_mean_ia_scales_inversely_with_rho():
    w = 1.0e9
    ia_full = gen_arrivals.mean_ia_ns(w, 0.5, 101)
    ia_half = gen_arrivals.mean_ia_ns(w, 1.0, 101)
    assert abs(ia_full - 2 * ia_half) < 1e-6
    # Explicit formula check: W / (C * rho * (n-1)).
    assert abs(ia_full - w / (gen_arrivals.CAPACITY * 0.5 * 100)) < 1e-6


def test_mean_ia_rejects_bad_args():
    for bad in [
        lambda: gen_arrivals.mean_ia_ns(1.0, 0.0, 10),
        lambda: gen_arrivals.mean_ia_ns(1.0, -0.1, 10),
        lambda: gen_arrivals.mean_ia_ns(1.0, 0.5, 1),
    ]:
        try:
            bad()
        except ValueError:
            continue
        assert False, "expected ValueError"


def test_draw_arrivals_shape_and_determinism():
    a = gen_arrivals.draw_arrivals(random.Random(3), 50, 1000.0)
    b = gen_arrivals.draw_arrivals(random.Random(3), 50, 1000.0)
    assert a == b
    assert len(a) == 50
    assert a[0] == 0
    assert all(a[i] <= a[i + 1] for i in range(len(a) - 1))


def test_draw_arrivals_scales_linearly_in_mean_ia():
    base = gen_arrivals.draw_arrivals(random.Random(5), 30, 1000.0)
    scaled = gen_arrivals.draw_arrivals(random.Random(5), 30, 2000.0)
    # Same underlying uniforms => times scale ~2x (modulo ns rounding).
    assert scaled[-1] >= base[-1]
    assert abs(scaled[-1] - 2 * base[-1]) <= 30  # within rounding slack


def _read_arrivals(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def test_main_uniform_writes_valid_trace():
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "cell")
        rc = gen_arrivals.main(
            [
                "--rho",
                "0.5",
                "--n",
                "200",
                "--uniform-svc-ns",
                "1000",
                "--seed",
                "1",
                "--out",
                out,
            ]
        )
        assert rc == 0
        rows = _read_arrivals(os.path.join(out, "arrivals.csv"))
        assert len(rows) == 200
        assert list(rows[0].keys()) == [
            "job_id",
            "arrival_time_ns",
            "num_ranks",
            "shape",
            "num_iterations",
        ]
        assert rows[0]["arrival_time_ns"] == "0"
        assert all(r["num_iterations"] == "1" for r in rows)
        ts = [int(r["arrival_time_ns"]) for r in rows]
        assert ts == sorted(ts)
        for r in rows:
            shape = shapes.parse_shape(r["shape"])
            assert shapes.is_legal("bw", *shape)
            assert shape[0] * shape[1] * shape[2] == int(r["num_ranks"])


def test_main_uniform_work_matches_config():
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "cell")
        gen_arrivals.main(
            [
                "--rho",
                "0.5",
                "--n",
                "150",
                "--uniform-svc-ns",
                "10",
                "--seed",
                "2",
                "--out",
                out,
            ]
        )
        rows = _read_arrivals(os.path.join(out, "arrivals.csv"))
        total_ranks = sum(int(r["num_ranks"]) for r in rows)
        cfg = {}
        with open(os.path.join(out, "trace_config.txt")) as f:
            for line in f:
                k, v = line.rstrip("\n").split("=", 1)
                cfg[k] = v
        # W = sum(ranks) * 10 * 1 (uniform svc, iters=1).
        assert abs(float(cfg["realized_work_npu_ns"]) - total_ranks * 10) < 1e-6
        assert "mean_ia_ns" in cfg


def test_main_deterministic_bytes():
    with tempfile.TemporaryDirectory() as d:
        a = os.path.join(d, "a")
        b = os.path.join(d, "b")
        argv = [
            "--rho",
            "0.7",
            "--n",
            "120",
            "--uniform-svc-ns",
            "500",
            "--seed",
            "9",
            "--out",
        ]
        gen_arrivals.main(argv + [a])
        gen_arrivals.main(argv + [b])
        with (
            open(os.path.join(a, "arrivals.csv")) as fa,
            open(os.path.join(b, "arrivals.csv")) as fb,
        ):
            assert fa.read() == fb.read()


def test_main_requires_a_service_source():
    with tempfile.TemporaryDirectory() as d:
        try:
            gen_arrivals.main(["--rho", "0.5", "--out", os.path.join(d, "x")])
        except SystemExit as e:
            assert e.code != 0
            return
        assert False, (
            "expected SystemExit when neither --svc nor --uniform-svc-ns given"
        )


def test_main_creates_jobs_symlinks_with_traces():
    with tempfile.TemporaryDirectory() as d:
        # Fake trace library: bw/<SHAPE>/chakra_trace.0.et for the shapes used.
        traces = os.path.join(d, "lib")
        out = os.path.join(d, "cell")
        # Pre-draw to learn which shapes appear, then materialize fake traces.
        jobs = gen_arrivals.build_job_sequence(random.Random(4), 40, "bw", 0.5)
        for _size, shape, _it in jobs:
            sd = os.path.join(traces, "bw", shapes.fmt_shape(shape))
            os.makedirs(sd, exist_ok=True)
            open(os.path.join(sd, "chakra_trace.0.et"), "w").close()
        rc = gen_arrivals.main(
            [
                "--rho",
                "0.5",
                "--n",
                "40",
                "--uniform-svc-ns",
                "100",
                "--seed",
                "4",
                "--out",
                out,
                "--traces",
                traces,
            ]
        )
        assert rc == 0
        link = os.path.join(out, "jobs", "0")
        assert os.path.islink(link)
        assert os.path.basename(os.path.realpath(link)) == shapes.fmt_shape(jobs[0][1])


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
