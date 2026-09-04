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

import cluster
import gen_arrivals
import shapes
import sweep


def _mkroot(d: str, dims: tuple[int, int, int] = (16, 16, 16)) -> str:
    cluster.save(d, dims)
    return d


def test_loads_grid():
    assert sweep.LOADS[0] == "0.05"
    assert sweep.LOADS[-1] == "1.00"
    assert len(sweep.LOADS) == 20


def test_experiment_table():
    with tempfile.TemporaryDirectory() as root:
        exps = sweep.experiments(_mkroot(root))
        assert len(exps) == 7
        for exp, flags in exps.items():
            assert exp.endswith("-load-sweep")
            # the size embedded in the name matches the --size-max flag
            dist = exp.split("-")[1]  # e.g. "pareto2048" / "uniform2048"
            size = dist.removeprefix("pareto").removeprefix("uniform")
            assert flags[flags.index("--size-max") + 1] == size
            # max job size tiers are half and quarter of the 4096 capacity
            assert int(size) in (2048, 1024)
            # pareto experiments pin alpha=0.5; the uniform one has no alpha
            if "uniform" in exp:
                assert "--size-dist" in flags
            else:
                assert flags[flags.index("--alpha") + 1] == "0.5"


def test_experiments_scale_with_cluster_dims():
    with tempfile.TemporaryDirectory() as root:
        exps = sweep.experiments(_mkroot(root, (8, 8, 8)))
        assert "easy-pareto256-load-sweep" in exps
        assert "easy-pareto128-load-sweep" in exps
        assert "easy-uniform128-load-sweep" in exps
        assert exps["easy-pareto256-load-sweep"][-1] == "256"


def test_experiments_require_cluster_json():
    with tempfile.TemporaryDirectory() as root:
        try:
            sweep.experiments(root)
            raise AssertionError("expected SystemExit")
        except SystemExit as e:
            assert "prereq" in str(e)


def test_admission_variants_share_trace_flags():
    with tempfile.TemporaryDirectory() as root:
        table = sweep.experiments(_mkroot(root))
        for family, count in [("pareto2048", 3), ("pareto1024", 3)]:
            exps = [e for e in table if family in e]
            assert len(exps) == count
            assert len({tuple(table[e]) for e in exps}) == 1


def test_combos_naming_and_count():
    cells = sweep.combos("swf-pareto2048-load-sweep")
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
            _mkroot(root)
            open(os.path.join(root, "service_times.csv"), "w").close()
            exp = "easy-uniform1024-load-sweep"
            sweep.gen(exp, root=root)
            assert len(calls) == len(sweep.PLACEMENTS) * len(sweep.LOADS)
            rhos = {a[a.index("--rho") + 1] for a in calls}
            assert rhos == set(sweep.LOADS)
            for a in calls:
                assert a[a.index("--n") + 1] == str(sweep.N_JOBS)
                assert a[a.index("--seed") + 1] == str(sweep.SEED)
                assert a[a.index("--cluster-dims") + 1] == "16x16x16"
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
        _mkroot(root)
        exp = next(iter(sweep.experiments(root)))
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


def _mkconfigs(
    root: str, npus: int = 4096, bw: float = 50.0, lt: float = 1000.0
) -> str:
    """A minimal configs/network.yml: npus_count sync + schedule-build source."""
    os.makedirs(os.path.join(root, "configs"), exist_ok=True)
    p = os.path.join(root, "configs", "network.yml")
    with open(p, "w") as f:
        f.write(
            f"topology: [ FullyConnected ]\nnpus_count: [ {npus} ]\n"
            f"bandwidth: [ {bw} ]  # GB/s\nlatency: [ {lt} ]  # ns\n"
        )
    return p


def test_sync_network_yml_follows_cluster_json():
    with tempfile.TemporaryDirectory() as root:
        p = _mkconfigs(root, npus=512)
        sweep.sync_network_yml(root, 4096)
        content = open(p).read()
        assert "npus_count: [ 4096 ]" in content
        assert "topology: [ FullyConnected ]" in content  # other lines untouched
        before = os.path.getmtime(p)
        sweep.sync_network_yml(root, 4096)  # already in sync -> no rewrite
        assert os.path.getmtime(p) == before


def test_build_schedules_from_yml_values():
    with tempfile.TemporaryDirectory() as root:
        _mkconfigs(root, bw=25.0, lt=700.0)
        bw = os.path.join(root, "configs", "bandwidth_schedule_torus.txt")
        lt = os.path.join(root, "configs", "latency_schedule_torus.txt")
        sweep.build_schedules(root, (2, 2, 4))  # nothing prebuilt -> build
        rows = open(bw).readlines()
        assert rows[0] == "BW 0\n" and rows[-1] == "END\n"
        assert len(rows) == 16 + 2 and len(rows[1].split()) == 16
        assert set(rows[1].split()) == {"0", "25"}  # per-link bw from the yml
        assert set(open(lt).readlines()[1].split()) == {"0", "700"}
        before = os.path.getmtime(bw)
        sweep.build_schedules(root, (2, 2, 4))  # matches yml + dims -> skipped
        assert os.path.getmtime(bw) == before
        _mkconfigs(root, bw=40.0, lt=700.0)  # yml bw changed -> rebuilt
        sweep.build_schedules(root, (2, 2, 4))
        assert set(open(bw).readlines()[1].split()) == {"0", "40"}


def test_build_schedules_fullmesh_for_ideal():
    with tempfile.TemporaryDirectory() as root:
        _mkconfigs(root, bw=25.0, lt=700.0)
        sweep.build_schedules(root, (2, 2, 4))
        mesh_bw = os.path.join(root, "configs", "bandwidth_schedule_fullmesh.txt")
        rows = open(mesh_bw).readlines()
        assert rows[0] == "BW 0\n" and len(rows) == 16 + 2
        # every off-diagonal cell nonzero (the binary's --fullmesh validation)
        for i, r in enumerate(rows[1:-1]):
            vals = r.split()
            assert vals[i] == "0" and vals.count("0") == 1
            assert set(vals) == {"0", "25"}
        before = os.path.getmtime(mesh_bw)
        sweep.build_schedules(root, (2, 2, 4))  # unchanged -> skipped
        assert os.path.getmtime(mesh_bw) == before
        # a torus matrix in the fullmesh slot is detected and rebuilt
        import gen_matrices

        gen_matrices.write_matrix(mesh_bw, "BW", 16, 2, 2, 4, 25, "torus")
        sweep.build_schedules(root, (2, 2, 4))
        assert open(mesh_bw).readlines()[1].split().count("0") == 1


def test_run_combo_policy_settings():
    import run_combo

    assert run_combo.policy_settings("sfc") == ("sfc", "torus", [])
    assert run_combo.policy_settings("firstfit") == ("firstfit", "torus", [])
    assert run_combo.policy_settings("ideal") == ("sfc", "fullmesh", ["--fullmesh"])
    assert run_combo.policy_settings("rfoldb2") == (
        "rfold",
        "torus",
        ["--block-size=2x2x2"],
    )


def test_blocksize_experiment_placements():
    with tempfile.TemporaryDirectory() as root:
        _mkroot(root, (8, 8, 8))
        exp = "fifo-pareto128-blocksize-load-sweep"
        assert exp in sweep.experiments(root)
        assert sweep.placements(exp, root) == [
            "firstfit",
            "rfoldb1",
            "rfoldb2",
            "rfoldb4",
            "rfoldb8",
            "ideal",
        ]
        cells = sweep.combos(exp, root)
        assert len(cells) == 6 * len(sweep.LOADS)
        assert ("fifo-rfoldb2-load0.05", "0.05") in cells
        # block sizes scale with the cluster: 16^3 adds rfoldb16
        _mkroot(root, (16, 16, 16))
        assert "rfoldb16" in sweep.placements(exp, root)
        # non-blocksize experiments keep the full placement list
        assert sweep.placements("fifo-pareto128-load-sweep", root) == sweep.PLACEMENTS


def _size(s: tuple[int, int, int]) -> int:
    return s[0] * s[1] * s[2]


def test_prereq_probes_builds_tracelib_and_measures_only_missing():
    traced, ran = [], []
    real_traces, real_farm = sweep.gen_traces.main, sweep.farm

    def fake_run(script, args, items, chunk, timeout):
        """Stand-in for farm()'s runner: every probe places, every measure
        reports svc=size*10, one batch at a time."""
        ran.append((script, list(items)))
        for i in range(0, len(items), chunk):
            batch = items[i : i + chunk]
            if script == "placeability.py":
                out = "shape,outcome\n" + "".join(f"{s},placed\n" for s in batch)
            else:
                out = "shape,size,svc_per_iter_ns\n" + "".join(
                    f"{s},{n},{n * 10}\n"
                    for s in batch
                    for n in [_size(shapes.parse_shape(s))]
                )
            yield "local", batch, out, ""

    def fake_traces(argv):
        traced.append(argv)
        # "build" a trace for every requested shape (standard + extra file)
        extra = argv[argv.index("--extra-shapes-file") + 1]
        want = set(shapes.all_legal_shapes("bw"))
        want |= {shapes.parse_shape(ln.strip()) for ln in open(extra) if ln.strip()}
        for sh in want:
            d = os.path.join(argv[argv.index("--out") + 1], "bw", shapes.fmt_shape(sh))
            os.makedirs(d, exist_ok=True)
            open(os.path.join(d, "chakra_trace.0.et"), "w").close()
        return 0

    sweep.gen_traces.main = fake_traces
    sweep.farm = lambda root, workers_file: fake_run
    try:
        with tempfile.TemporaryDirectory() as root:
            # small dims: prereq (re)generates real NxN schedule matrices
            _mkroot(root, dims=(2, 2, 2))
            net_yml = _mkconfigs(root, npus=512)
            sweep.prereq(root=root)
            # the user's dims answer is synced into network.yml...
            assert "npus_count: [ 8 ]" in open(net_yml).read()
            # ...and the BW/LT schedule matrices are built to match
            bw = os.path.join(root, "configs", "bandwidth_schedule_torus.txt")
            row = open(bw).readlines()[1].split()
            assert len(row) == 8 and set(row) == {"0", "50"}
            assert traced[0][traced[0].index("--cluster-dims") + 1] == "2x2x2"
            # probe ran over the expanded universe at the quarter cap (2)...
            assert ran[0][0] == "placeability.py"
            assert sorted(ran[0][1]) == sorted(
                shapes.fmt_shape(s) for s in shapes.expanded_legal_shapes("bw", 2)
            )
            assert os.path.isfile(os.path.join(root, "rfold_placeable2.txt"))
            # ...then the measure covered standard + placeable, largest first
            assert ran[1][0] == "measure_svc.py"
            sizes = [_size(shapes.parse_shape(s)) for s in ran[1][1]]
            assert sizes == sorted(sizes, reverse=True)
            table = os.path.join(root, "service_times.csv")
            have = sweep.measure_svc.read_table(table)
            assert len(have) == len(ran[1][1]) and have[(2, 2, 2)] == 80
            # a shape dropped from the table -> only that one is re-measured
            del have[(1, 1, 2)]
            sweep.measure_svc.write_table(
                table, [(s, _size(s), v) for s, v in have.items()]
            )
            sweep.prereq(root=root)
            assert ran[2] == (
                "measure_svc.py",
                ["1x1x2"],
            )  # probe file cached: no re-probe
            assert sweep.measure_svc.read_table(table)[(1, 1, 2)] == 20
            sweep.prereq(root=root)  # complete: nothing runs
            assert len(ran) == 3 and len(traced) == 3
    finally:
        sweep.gen_traces.main, sweep.farm = real_traces, real_farm


def test_progress_table_cells():
    exp = "swf-pareto2048-load-sweep"
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
    # combos with no snapshot yet show 0; ideal is the last placement row
    assert lines[-1].strip().startswith("ideal") and lines[-1].rstrip().endswith("0")


def test_trace_len_counts_arrivals_rows():
    exp = "easy-uniform1024-load-sweep"
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
