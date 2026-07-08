"""Self-running tests for scripts/launcher.py (worker probe + packing)."""

import os
import sys
import tempfile

sys.path.insert(
    0,
    os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
    ),
)

import launcher
import sweep


def test_parse_workers_missing_and_empty_mean_local():
    with tempfile.TemporaryDirectory() as d:
        assert launcher.parse_workers(os.path.join(d, "nope.txt")) == []
        p = os.path.join(d, "workers.txt")
        with open(p, "w") as f:
            f.write("# only comments\n\n   \n")
        assert launcher.parse_workers(p) == []


def test_parse_workers_lines():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "workers.txt")
        with open(p, "w") as f:
            f.write("# farm\nalice@h1.example.org\n\n bob@10.0.0.2 \n")
        assert launcher.parse_workers(p) == ["alice@h1.example.org", "bob@10.0.0.2"]


def test_slot_count_bounds():
    assert launcher.slot_count(64, 251) == 10  # 251 // 24, cpu bound not hit
    assert launcher.slot_count(8, 1024) == 6  # cpu bound: 8 - 2
    assert launcher.slot_count(64, 12) == 0  # not enough mem for one sim
    assert launcher.slot_count(1, 100) == 0  # no cpu headroom


def test_probe_local():
    cpus, mem_gb = launcher.probe(launcher.LOCAL)
    assert cpus >= 1 and mem_gb >= 0


def test_plan_assignments_balances_by_slots():
    combos = sweep.combos("easy-pareto512-load-sweep")  # 120 cells
    host_slots = {"a": 2, "b": 1}
    rows = launcher.plan_assignments(combos, host_slots)
    assert len(rows) == len(combos)
    assert {c for c, _, _ in rows} == {c for c, _ in combos}
    work = {"a": 0.0, "b": 0.0}
    for _, load, host in rows:
        work[host] += float(load)
    # work split should track the 2:1 slot ratio
    assert 1.5 < work["a"] / work["b"] < 2.5, work
    # deterministic
    assert rows == launcher.plan_assignments(combos, host_slots)


def test_plan_assignments_single_host_gets_everything():
    combos = [("c-load0.10", "0.10"), ("c-load0.90", "0.90")]
    rows = launcher.plan_assignments(combos, {"local": 4})
    assert [(r[0], r[2]) for r in rows] == [
        ("c-load0.10", "local"),
        ("c-load0.90", "local"),
    ]


def test_assignments_roundtrip():
    rows = [("easy-firstfit-load0.05", "0.05", "a@h"), ("x", "1.00", "local")]
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "assignments.csv")
        launcher.write_assignments(p, rows)
        assert launcher.read_assignments(p) == rows


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
