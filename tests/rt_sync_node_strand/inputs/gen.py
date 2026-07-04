#!/usr/bin/env python3
"""Generate inputs for the sync-node-strand regression test.

Synthesizes two single-rank Chakra traces (a 4-node COMP chain each):
  - strand: the root COMP has tensor_size == 0, so issue_comp() completes it
    synchronously via skip_invalid() -- no event is registered. Before the
    fix-point loop in Workload::issue_dep_free_nodes, the chain's remaining
    nodes were never issued and the job stalled until global quiescence.
  - normal: identical chain with a nonzero root.

Also emits a 4x4x4 torus BW/LT schedule (6 neighbors per node, zero
elsewhere) and the arrivals CSV: the strand job arrives first and is ~3x
shorter than the normal job, so a strand is detectable as the strand job
finishing *after* the normal job.

Run with PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python (the in-repo
et_def_pb2.py predates protoc 3.19 codegen).
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "extern/graph_frontend/chakra/schema/protobuf"))

import et_def_pb2 as pb  # noqa: E402
from google.protobuf.internal import encoder  # noqa: E402

DIMS = (4, 4, 4)
N = DIMS[0] * DIMS[1] * DIMS[2]
BW_GBPS = 50
LT_NS = 500
CHAIN_LEN = 4
NUM_OPS = 1342177280  # ~1.3e9 MACs -> milliseconds per node under roofline
TENSOR_SIZE = 268435456


def write_msgs(path, msgs):
    with open(path, "wb") as f:
        for m in msgs:
            blob = m.SerializeToString()
            f.write(encoder._VarintBytes(len(blob)))
            f.write(blob)


def comp_node(node_id, dep, tensor_size):
    n = pb.Node()
    n.id = node_id
    n.name = f"comp_{node_id}"
    n.type = pb.COMP_NODE
    if dep is not None:
        n.data_deps.append(dep)
    a = n.attr.add()
    a.name = "num_ops"
    a.int64_val = NUM_OPS
    a = n.attr.add()
    a.name = "tensor_size"
    a.uint64_val = tensor_size
    a = n.attr.add()
    a.name = "is_cpu_op"
    a.int32_val = 0
    return n


def make_trace(out_dir, root_tensor_size):
    os.makedirs(out_dir, exist_ok=True)
    meta = pb.GlobalMetadata()
    a = meta.attr.add()
    a.name = "schema"
    a.string_val = "symbolic_tensor_network"
    msgs = [meta]
    for i in range(1, CHAIN_LEN + 1):
        ts = root_tensor_size if i == 1 else TENSOR_SIZE
        msgs.append(comp_node(i, i - 1 if i > 1 else None, ts))
    write_msgs(os.path.join(out_dir, "chakra_trace.0.et"), msgs)
    with open(os.path.join(out_dir, "comm_group.json"), "w") as f:
        f.write("{}\n")


def torus_neighbors(i):
    dx, dy, dz = DIMS
    x, y, z = i % dx, (i // dx) % dy, i // (dx * dy)
    for cx, cy, cz in [(1, 0, 0), (0, 1, 0), (0, 0, 1)]:
        for s in (1, -1):
            nx = (x + s * cx) % dx
            ny = (y + s * cy) % dy
            nz = (z + s * cz) % dz
            yield nz * dy * dx + ny * dx + nx


def write_schedule(path, tag, val):
    with open(path, "w") as f:
        f.write(f"{tag} 0\n")
        for i in range(N):
            row = [0] * N
            for j in torus_neighbors(i):
                row[j] = val
            f.write(" ".join(str(v) for v in row) + "\n")
        f.write("END\n")


def main():
    gen = os.path.join(HERE, "..", "outputs", "gen")
    os.makedirs(gen, exist_ok=True)
    make_trace(os.path.join(gen, "trace_strand"), 0)
    make_trace(os.path.join(gen, "trace_normal"), TENSOR_SIZE)
    jobs = os.path.join(gen, "jobs")
    os.makedirs(jobs, exist_ok=True)
    for jid, tdir in (("0", "trace_strand"), ("1", "trace_normal")):
        link = os.path.join(jobs, jid)
        if os.path.lexists(link):
            os.remove(link)
        os.symlink(os.path.join("..", tdir), link)
    with open(os.path.join(gen, "arrivals.csv"), "w") as f:
        f.write("job_id,arrival_time_ns,num_ranks,shape,num_iterations\n")
        f.write("0,0,1,1x1x1,1\n")  # strand candidate, short
        f.write("1,1000,1,1x1x1,3\n")  # normal, ~3x longer
    write_schedule(os.path.join(gen, "bandwidth_schedule.txt"), "BW", BW_GBPS)
    write_schedule(os.path.join(gen, "latency_schedule.txt"), "LT", LT_NS)
    print(f"generated inputs under {os.path.abspath(gen)}")


if __name__ == "__main__":
    main()
