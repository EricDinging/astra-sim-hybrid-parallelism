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

shapes.init((16, 16, 16))
DIMS_FLAG = ["--cluster-dims", "16x16x16"]


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


def test_sample_duration_in_range():
    rng = random.Random(0)
    for _ in range(5000):
        d = gen_arrivals.sample_duration(rng, 20, 1.3, 1.2)
        assert 1 <= d <= 20


def test_sample_duration_max_iters_one_is_constant():
    rng = random.Random(0)
    assert all(gen_arrivals.sample_duration(rng, 1, 1.3, 1.2) == 1 for _ in range(50))


def test_sample_duration_median_is_small():
    # Log-normal with a small median => most jobs short => median near the floor.
    rng = random.Random(7)
    draws = sorted(gen_arrivals.sample_duration(rng, 20, 1.3, 1.2) for _ in range(4001))
    assert draws[2000] <= 2


def test_sample_duration_mass_concentrates_at_floor():
    # median 1.3 => roughly half the jobs are exactly 1 iteration.
    rng = random.Random(11)
    draws = [gen_arrivals.sample_duration(rng, 20, 1.3, 1.2) for _ in range(5000)]
    frac_one = sum(1 for d in draws if d == 1) / len(draws)
    assert 0.45 <= frac_one <= 0.65


def test_sample_duration_larger_sigma_has_heavier_tail():
    # Larger sigma thickens the tail => larger mean duration.
    rng_a = random.Random(2)
    rng_b = random.Random(2)
    mean_heavy = sum(
        gen_arrivals.sample_duration(rng_a, 20, 1.3, 2.0) for _ in range(5000)
    )
    mean_light = sum(
        gen_arrivals.sample_duration(rng_b, 20, 1.3, 0.6) for _ in range(5000)
    )
    assert mean_heavy > mean_light


def test_sample_duration_rejects_bad_args():
    rng = random.Random(0)
    for bad in [
        lambda: gen_arrivals.sample_duration(rng, 0, 1.3, 1.2),
        lambda: gen_arrivals.sample_duration(rng, 20, 0.0, 1.2),
        lambda: gen_arrivals.sample_duration(rng, 20, 1.3, 0.0),
        lambda: gen_arrivals.sample_duration(rng, 20, 1.3, -1.0),
    ]:
        try:
            bad()
        except ValueError:
            continue
        assert False, "expected ValueError"


def test_build_job_sequence_shapes_match_size_and_legal():
    rng = random.Random(7)
    jobs = gen_arrivals.build_job_sequence(rng, 300, "bw", 0.5)
    assert len(jobs) == 300
    for size, shape, iters in jobs:
        assert shapes.is_legal("bw", *shape)
        assert shape[0] * shape[1] * shape[2] == size
        assert 1 <= iters <= 20


def test_build_job_sequence_deterministic():
    a = gen_arrivals.build_job_sequence(random.Random(42), 100, "bw", 0.5)
    b = gen_arrivals.build_job_sequence(random.Random(42), 100, "bw", 0.5)
    assert a == b


def test_build_job_sequence_uniform_size_dist():
    pareto = gen_arrivals.build_job_sequence(random.Random(7), 500, "bw", 0.5)
    uniform = gen_arrivals.build_job_sequence(
        random.Random(7), 500, "bw", 0.5, size_dist="uniform"
    )
    for size, shape, _iters in uniform:
        assert shapes.is_legal("bw", *shape)
        assert shape[0] * shape[1] * shape[2] == size
    # Uniform-over-index puts far more mass on large sizes than Pareto a=0.5.
    mean = lambda js: sum(s for s, _, _ in js) / len(js)  # noqa: E731
    assert mean(uniform) > 2 * mean(pareto)


def test_build_job_sequence_dims_restriction():
    jobs = gen_arrivals.build_job_sequence(
        random.Random(7), 200, "bw", 0.5, dims=frozenset({1, 4, 8})
    )
    for _size, shape, _iters in jobs:
        assert all(d in (1, 4, 8) for d in shape)


# the 8x8x8 rounded-experiment menu: descending shapes over {1,2,4} (sub-block
# tilers of the 4x4x4 rfold block) plus the two-block 8x4x4 brick
MENU = [
    (1, 1, 1),
    (2, 1, 1),
    (2, 2, 1),
    (2, 2, 2),
    (4, 1, 1),
    (4, 2, 1),
    (4, 2, 2),
    (4, 4, 1),
    (4, 4, 2),
    (4, 4, 4),
    (8, 4, 4),
]


def test_snap_shape_rounds_down_by_default():
    assert gen_arrivals.snap_shape((6, 1, 1), MENU) == (4, 1, 1)  # 6 -> 4, 1D
    assert gen_arrivals.snap_shape((6, 2, 1), MENU) == (4, 2, 1)  # 12 -> 8, 2D
    assert gen_arrivals.snap_shape((6, 6, 1), MENU) == (4, 4, 2)  # 36 -> 32


def test_snap_shape_rounds_up_to_whole_blocks():
    # within 4/3x of a whole-block multiple of the 4x4x4 block => up
    assert gen_arrivals.snap_shape((6, 4, 2), MENU) == (4, 4, 4)  # 48 -> 64
    assert gen_arrivals.snap_shape((6, 4, 4), MENU) == (8, 4, 4)  # 96 -> 128
    # 72 is NOT within 4/3x of 128 => down to 64
    assert gen_arrivals.snap_shape((6, 6, 2), MENU) == (4, 4, 4)


def test_snap_shape_keeps_dimensionality_close():
    assert gen_arrivals.snap_shape((1, 1, 2), MENU) == (2, 1, 1)  # canonical 1D
    assert gen_arrivals.snap_shape((8, 2, 1), MENU) == (4, 4, 1)  # 2D stays 2D
    assert gen_arrivals.snap_shape((2, 2, 4), MENU) == (4, 2, 2)  # 3D stays 3D
    # no 1D size-8 target: a 1D job crosses to the nearest dimensionality (2D)
    assert gen_arrivals.snap_shape((8, 1, 1), MENU) == (4, 2, 1)


def test_snap_shape_menu_shapes_are_fixed_points():
    for t in MENU:
        assert gen_arrivals.snap_shape(t, MENU) == t


def test_snap_shape_rejects_size_below_menu():
    try:
        gen_arrivals.snap_shape((1, 1, 1), [(2, 1, 1), (4, 2, 1)])
    except ValueError:
        return
    assert False, "expected ValueError when no menu size <= job size"


def test_build_job_sequence_snap_is_rounded_twin():
    base = gen_arrivals.build_job_sequence(
        random.Random(7), 300, "bw", 0.5, size_max=128
    )
    snapped = gen_arrivals.build_job_sequence(
        random.Random(7), 300, "bw", 0.5, size_max=128, snap=MENU
    )
    assert len(base) == len(snapped)
    for (bsize, bshape, bit), (ssize, sshape, sit) in zip(base, snapped):
        assert sit == bit  # rng draws stay aligned
        assert sshape in MENU
        assert sshape == gen_arrivals.snap_shape(bshape, MENU)
        # sizes round down, except up onto whole-block targets within 4/3x
        assert ssize == sshape[0] * sshape[1] * sshape[2]
        assert ssize <= bsize or (ssize % 64 == 0 and 3 * ssize <= 4 * bsize)


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
    ia_full = gen_arrivals.mean_ia_ns(w, 0.5, 101, 4096)
    ia_half = gen_arrivals.mean_ia_ns(w, 1.0, 101, 4096)
    assert abs(ia_full - 2 * ia_half) < 1e-6
    # Explicit formula check: W / (C * rho * (n-1)).
    assert abs(ia_full - w / (4096 * 0.5 * 100)) < 1e-6
    # Capacity halves => same offered work needs twice the arrival spacing...
    assert abs(gen_arrivals.mean_ia_ns(w, 0.5, 101, 2048) - 2 * ia_full) < 1e-6


def test_mean_ia_rejects_bad_args():
    for bad in [
        lambda: gen_arrivals.mean_ia_ns(1.0, 0.0, 10, 4096),
        lambda: gen_arrivals.mean_ia_ns(1.0, -0.1, 10, 4096),
        lambda: gen_arrivals.mean_ia_ns(1.0, 0.5, 1, 4096),
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
                *DIMS_FLAG,
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
        assert all(1 <= int(r["num_iterations"]) <= 20 for r in rows)
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
                *DIMS_FLAG,
            ]
        )
        rows = _read_arrivals(os.path.join(out, "arrivals.csv"))
        # W = sum_i ranks_i * 10 * iters_i (uniform svc=10).
        expected_w = sum(
            int(r["num_ranks"]) * 10 * int(r["num_iterations"]) for r in rows
        )
        cfg = {}
        with open(os.path.join(out, "trace_config.txt")) as f:
            for line in f:
                k, v = line.rstrip("\n").split("=", 1)
                cfg[k] = v
        assert abs(float(cfg["realized_work_npu_ns"]) - expected_w) < 1e-6
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
            *DIMS_FLAG,
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
            gen_arrivals.main(
                ["--rho", "0.5", "--out", os.path.join(d, "x"), *DIMS_FLAG]
            )
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
                *DIMS_FLAG,
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
