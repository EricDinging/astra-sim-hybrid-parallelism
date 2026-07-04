#!/usr/bin/env python3
"""Generate inputs for the cross-rank ordinal-divergence regression test.

Synthesizes two 2-rank jobs, each with two dependency-free ALL_REDUCE
collectives (A, B) on the same comm group:
  - aligned: both ranks number A=1, B=2. The Kahn ordinal scan (smallest
    dependency-free id first) yields the order [A, B] on both ranks; the job
    must run to completion.
  - divergent: rank 1 numbers them A=2, B=1, so its ordinal order is [B, A]
    while rank 0's is [A, B]. Per-group ordered admission (and the
    ordinal-derived stream ids/tags) require member ranks to agree; the
    fingerprint check in the Workload constructor must abort the run with a
    diagnostic instead of letting the job wedge.

Run with PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python (the in-repo
et_def_pb2.py predates protoc 3.19 codegen).
"""

import json
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
ALL_REDUCE = 2  # ChakraProtoMsg::CollectiveCommType
SIZE_A = 1 << 20
SIZE_B = 2 << 20


def write_msgs(path, msgs):
    with open(path, "wb") as f:
        for m in msgs:
            blob = m.SerializeToString()
            f.write(encoder._VarintBytes(len(blob)))
            f.write(blob)


def coll_node(node_id, name, comm_size):
    n = pb.Node()
    n.id = node_id
    n.name = name
    n.type = pb.COMM_COLL_NODE
    a = n.attr.add()
    a.name = "comm_type"
    a.int64_val = ALL_REDUCE
    a = n.attr.add()
    a.name = "comm_size"
    a.int64_val = comm_size
    a = n.attr.add()
    a.name = "pg_name"
    a.string_val = "0"
    a = n.attr.add()
    a.name = "is_cpu_op"
    a.int32_val = 0
    return n


def make_rank(out_dir, rank, id_of_a, id_of_b):
    meta = pb.GlobalMetadata()
    a = meta.attr.add()
    a.name = "schema"
    a.string_val = "symbolic_tensor_network"
    msgs = [
        meta,
        coll_node(id_of_a, "collA", SIZE_A),
        coll_node(id_of_b, "collB", SIZE_B),
    ]
    write_msgs(os.path.join(out_dir, f"chakra_trace.{rank}.et"), msgs)


def make_job(out_dir, rank1_swapped):
    os.makedirs(out_dir, exist_ok=True)
    make_rank(out_dir, 0, 1, 2)
    if rank1_swapped:
        make_rank(out_dir, 1, 2, 1)
    else:
        make_rank(out_dir, 1, 1, 2)
    with open(os.path.join(out_dir, "comm_group.json"), "w") as f:
        json.dump({"0": [0, 1]}, f)


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
    make_job(os.path.join(gen, "trace_aligned"), rank1_swapped=False)
    make_job(os.path.join(gen, "trace_divergent"), rank1_swapped=True)
    for variant in ("aligned", "divergent"):
        jobs = os.path.join(gen, f"jobs_{variant}")
        os.makedirs(jobs, exist_ok=True)
        link = os.path.join(jobs, "0")
        if os.path.lexists(link):
            os.remove(link)
        os.symlink(os.path.join("..", f"trace_{variant}"), link)
    with open(os.path.join(gen, "arrivals.csv"), "w") as f:
        f.write("job_id,arrival_time_ns,num_ranks,shape,num_iterations\n")
        f.write("0,0,2,1x1x2,1\n")
    write_schedule(os.path.join(gen, "bandwidth_schedule.txt"), "BW", BW_GBPS)
    write_schedule(os.path.join(gen, "latency_schedule.txt"), "LT", LT_NS)
    print(f"generated inputs under {os.path.abspath(gen)}")


if __name__ == "__main__":
    main()
