"""Self-running tests for scripts/sweep.py (the sweep-harness phase library)."""

import os
import sys
import tempfile

sys.path.insert(
    0,
    os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
    ),
)

import gen_arrivals
import shapes
import sweep


def test_loads_grid():
    assert sweep.LOADS[0] == "0.05"
    assert sweep.LOADS[-1] == "1.00"
    assert len(sweep.LOADS) == 20


def test_experiment_table():
    assert len(sweep.EXPERIMENTS) == 6
    for exp, flags in sweep.EXPERIMENTS.items():
        assert exp.endswith("-load-sweep")
        assert "--size-max" in flags
        # pareto experiments pin alpha=0.5; the uniform one has no alpha
        if "uniform" in exp:
            assert "--size-dist" in flags
        else:
            assert flags[flags.index("--alpha") + 1] == "0.5"


def test_admission_variants_share_trace_flags():
    p512 = [e for e in sweep.EXPERIMENTS if "pareto512" in e]
    assert len(p512) == 3
    assert len({tuple(sweep.EXPERIMENTS[e]) for e in p512}) == 1


def test_combos_naming_and_count():
    cells = sweep.combos("swf-pareto512-load-sweep")
    assert len(cells) == len(sweep.PLACEMENTS) * len(sweep.LOADS)
    assert ("swf-firstfit-load0.05", "0.05") in cells
    assert ("swf-random-load1.00", "1.00") in cells
    names = [c for c, _ in cells]
    assert len(set(names)) == len(names)


def _fake_gen(calls):
    def fake_main(argv):
        out = argv[argv.index("--out") + 1]
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, "arrivals.csv"), "w").close()
        calls.append(argv)
        return 0

    return fake_main


def test_gen_generates_each_combo_then_skips():
    calls = []
    real_main = gen_arrivals.main
    gen_arrivals.main = _fake_gen(calls)
    try:
        with tempfile.TemporaryDirectory() as root:
            open(os.path.join(root, "service_times.csv"), "w").close()
            exp = "easy-uniform512-load-sweep"
            sweep.gen(exp, root=root)
            assert len(calls) == len(sweep.PLACEMENTS) * len(sweep.LOADS)
            rhos = {a[a.index("--rho") + 1] for a in calls}
            assert rhos == set(sweep.LOADS)
            for a in calls:
                assert a[a.index("--n") + 1] == str(sweep.N_JOBS)
                assert a[a.index("--seed") + 1] == str(sweep.SEED)
                assert "--size-dist" in a and "uniform" in a
                out = a[a.index("--out") + 1]
                assert os.path.basename(os.path.dirname(out)) == exp
                assert os.path.basename(out).startswith("easy-")
            sweep.gen(exp, root=root)  # second run: everything skips
            assert len(calls) == len(sweep.PLACEMENTS) * len(sweep.LOADS)
    finally:
        gen_arrivals.main = real_main


def test_gen_requires_service_times():
    with tempfile.TemporaryDirectory() as root:
        try:
            sweep.gen("easy-pareto512-load-sweep", root=root)
            raise AssertionError("expected SystemExit")
        except SystemExit as e:
            assert "service_times" in str(e)


def test_clean_removes_experiments_keeps_prereqs():
    with tempfile.TemporaryDirectory() as root:
        exp = next(iter(sweep.EXPERIMENTS))
        cell = os.path.join(root, "runs", exp, "easy-firstfit-load0.05")
        os.makedirs(cell)
        open(os.path.join(cell, "arrivals.csv"), "w").close()
        legacy = os.path.join(root, "runs", "pareto-1to512")
        os.makedirs(legacy)
        os.makedirs(os.path.join(root, "tracelib"))
        open(os.path.join(root, "service_times.csv"), "w").close()
        sweep.clean(root=root)
        assert not os.path.exists(os.path.join(root, "runs", exp))
        assert os.path.isdir(legacy)  # only harness-owned dirs are removed
        # prerequisites survive a clean
        assert os.path.isfile(os.path.join(root, "service_times.csv"))
        assert os.path.isdir(os.path.join(root, "tracelib"))


def test_prereq_builds_tracelib_and_skips_existing_svc():
    traced, measured = [], []
    real_traces, real_svc = sweep.gen_traces.main, sweep.svc
    sweep.gen_traces.main = lambda argv: (traced.append(argv), 0)[1]
    sweep.svc = lambda root: measured.append(root)
    try:
        with tempfile.TemporaryDirectory() as root:
            sweep.prereq(root=root)  # no svc table yet -> measures
            assert traced[0][traced[0].index("--out") + 1].endswith("tracelib")
            assert measured == [root]
            table = os.path.join(root, "service_times.csv")
            with open(table, "w") as f:
                f.write("shape,size,svc_per_iter_ns\n2x2x2,8,123\n")
            sweep.prereq(root=root)  # partial (failed measure) -> re-measures
            assert len(measured) == 2
            with open(table, "w") as f:
                f.write("shape,size,svc_per_iter_ns\n")
                for s in shapes.all_legal_shapes("bw"):
                    f.write(f"{shapes.fmt_shape(s)},{s[0] * s[1] * s[2]},123\n")
            sweep.prereq(root=root)  # complete table -> tracelib only
            assert len(traced) == 3 and len(measured) == 2
    finally:
        sweep.gen_traces.main, sweep.svc = real_traces, real_svc


def test_progress_table_cells():
    exp = "swf-pareto512-load-sweep"
    cells = {
        "swf-firstfit-load0.05": (20, "rc=0"),
        "swf-firstfit-load0.10": (7, "-"),
        "swf-rfold-load0.05": (3, "rc=137"),
    }
    tbl = sweep.progress_table(exp, cells, 20)
    lines = tbl.splitlines()
    assert lines[0].startswith(exp)
    assert len(lines) == 2 + len(sweep.PLACEMENTS)
    ff = next(ln for ln in lines if ln.strip().startswith("firstfit"))
    assert "✓" in ff and " 7" in ff
    rf = next(ln for ln in lines if ln.strip().startswith("rfold"))
    assert "✗137" in rf
    # combos with no snapshot yet show 0
    assert lines[-1].strip().startswith("random") and lines[-1].rstrip().endswith("0")


def test_trace_len_counts_arrivals_rows():
    exp = "easy-uniform512-load-sweep"
    with tempfile.TemporaryDirectory() as root:
        combo, _ = sweep.combos(exp)[0]
        d = os.path.join(root, "runs", exp, combo)
        os.makedirs(d)
        with open(os.path.join(d, "arrivals.csv"), "w") as f:
            f.write("job_id,arrival_time_ns\n0,0\n1,5\n")
        assert sweep._trace_len(exp, root) == 2
        assert sweep._trace_len(exp, "/nonexistent") == sweep.N_JOBS


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
