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
    # rs630-like: 40 cpus, 256 GB -> 85% budget = 217 GB / 3 = 72, cpu-bound
    assert launcher.slot_count(40, 256, 3) == 38
    assert launcher.slot_count(64, 251, 24) == 8  # 251*0.85 // 24, mem-bound
    assert launcher.slot_count(64, 2, 3) == 0  # not enough mem for one sim
    assert launcher.slot_count(1, 100, 3) == 0  # no cpu headroom


def test_mem_per_run_two_tier_at_512_and_4096_legacy_elsewhere():
    assert launcher.mem_per_run_gb(512) == 6.5
    assert launcher.mem_per_run_gb(512, ["fifo-large256to512-load-sweep"]) == 10
    assert launcher.mem_per_run_gb(4096) == 10
    assert launcher.mem_per_run_gb(4096, ["fifo-pareto1024-load-sweep"]) == 10
    for heavy in ("fifo-large512to1024", "fifo-uniform1024", "fifo-pareto2048"):
        assert launcher.mem_per_run_gb(4096, [f"{heavy}-load-sweep"]) == 15
    assert launcher.mem_per_run_gb(2048) == 12  # legacy envelope
    assert launcher.mem_per_run_gb(256) == 4  # floor


def test_workspace_is_capacity_keyed():
    assert launcher.workspace(4096) == "/workspace/cluster4096"
    assert launcher.workspace(512) == "/workspace/cluster512"


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


def test_qname_sorts_lpt_and_roundtrips():
    names = [
        launcher.qname("e", f"easy-firstfit-load{ld}", ld)
        for ld in ("0.05", "1.00", "0.30")
    ]
    by_name = [n.split("--")[2] for n in sorted(names)]
    assert by_name == [
        "easy-firstfit-load1.00",
        "easy-firstfit-load0.30",
        "easy-firstfit-load0.05",
    ]


def test_plan_steals_backlogged_to_idle():
    # host a ran dry (2 slots, everything done); host b is backlogged
    # (1 slot busy, 3 queued); our exps = ["e"]
    snaps = {
        "a": {("e", "e-ff-load0.90"): (5, "rc=0")},
        "b": {
            ("e", "e-ff-load1.00"): (9, "run"),
            ("e", "e-ff-load0.05"): (0, "q"),
            ("e", "e-ff-load0.70"): (0, "q"),
            ("e", "e-rf-load0.50"): (0, "q"),
        },
    }
    moves = launcher.plan_steals(snaps, {"a": 2, "b": 1}, ["e"])
    # a has 2 free slots; b's surplus is 3 queued vs 0 free -> 2 moves,
    # longest loads first
    assert [(m[1], m[3], m[4]) for m in moves] == [
        ("e-ff-load0.70", "b", "a"),
        ("e-rf-load0.50", "b", "a"),
    ]


def test_plan_steals_skips_legacy_local_unprobed_and_foreign():
    snaps = {
        # legacy '-' entry: slot accounting unknown -> whole host skipped
        "a": {("e", "e-ff-load0.10"): (0, "-")},
        # unprobed host -> skipped
        "c": {("e", "e-ff-load0.20"): (0, "q")},
        # local -> skipped
        launcher.LOCAL: {("e", "e-ff-load0.30"): (0, "q")},
        # foreign experiment's queued combo never moves
        "b": {("x", "x-ff-load0.90"): (0, "q"), ("e", "e-ff-load0.40"): (0, "q")},
        "idle": {("e", "e-rf-load0.60"): (7, "rc=0")},
    }
    host_slots = {"a": 4, "b": 1, "idle": 4, launcher.LOCAL: 4}
    moves = launcher.plan_steals(snaps, host_slots, ["e"])
    # b is backlogged (2 queued, 1 slot) but only its own-exp combo moves
    assert [(m[0], m[1], m[4]) for m in moves] == [("e", "e-ff-load0.40", "idle")]


def test_plan_steals_nothing_when_balanced():
    snaps = {
        "a": {("e", "e-ff-load0.90"): (1, "run")},
        "b": {("e", "e-ff-load0.80"): (1, "run"), ("e", "e-ff-load0.10"): (0, "q")},
    }
    assert launcher.plan_steals(snaps, {"a": 1, "b": 2}, ["e"]) == []


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
