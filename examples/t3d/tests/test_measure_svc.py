"""Self-running smoke tests for measure_svc.py (pure helpers only; the binary
path is exercised via a real measurement run). Run: python3 tests/test_measure_svc.py."""

import csv
import os
import sys
import tempfile

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts")
)

import measure_svc  # noqa: E402
import shapes  # noqa: E402

shapes.init((16, 16, 16))


def _touch_trace(traces, model, shape):
    sd = os.path.join(traces, model, shapes.fmt_shape(shape))
    os.makedirs(sd, exist_ok=True)
    open(os.path.join(sd, "chakra_trace.0.et"), "w").close()


def test_write_single_job_dir_5col_and_symlink():
    with tempfile.TemporaryDirectory() as d:
        traces = os.path.join(d, "lib")
        _touch_trace(traces, "bw", (2, 2, 2))
        work = os.path.join(d, "scratch")
        os.makedirs(work)
        size = measure_svc.write_single_job_dir(work, (2, 2, 2), traces, "bw")
        assert size == 8
        with open(os.path.join(work, "arrivals.csv"), newline="") as f:
            rows = list(csv.reader(f))
        assert rows[0] == [
            "job_id",
            "arrival_time_ns",
            "num_ranks",
            "shape",
            "num_iterations",
        ]
        assert rows[1] == ["0", "0", "8", "2x2x2", "1"]
        link = os.path.join(work, "jobs", "0")
        assert os.path.islink(link)
        assert os.path.basename(os.path.realpath(link)) == "2x2x2"


def test_legal_shapes_with_traces_filters_by_presence():
    with tempfile.TemporaryDirectory() as d:
        traces = os.path.join(d, "lib")
        _touch_trace(traces, "bw", (2, 2, 2))  # present
        got = measure_svc.legal_shapes_with_traces(
            "bw", traces, only=["2x2x2", "4x4x4"]
        )
        assert got == [(2, 2, 2)]


def test_write_table_header_and_sort():
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "service_times.csv")
        measure_svc.write_table(out, [((4, 1, 1), 4, 900), ((2, 2, 2), 8, 1500)])
        with open(out, newline="") as f:
            rows = list(csv.reader(f))
        assert rows[0] == ["shape", "size", "svc_per_iter_ns"]
        # sorted by (size, shape): size 4 before size 8.
        assert rows[1] == ["4x1x1", "4", "900"]
        assert rows[2] == ["2x2x2", "8", "1500"]


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
