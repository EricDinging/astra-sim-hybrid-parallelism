# cluster4096

Sweep scripts, configs, and results for the **4096-node cluster** experiments —
a 16×16×16 3-D torus. Everything is driven from a single top-level
`reproduce.sh` that dispatches to helpers in `scripts/`; re-running a phase
reproduces that artifact deterministically.

## Layout

- `reproduce.sh` — top-level driver. Run a phase, e.g. `./reproduce.sh traces`.
- `scripts/` — helper libraries called by `reproduce.sh`:
  - `shapes.py` — the authority on legal job shapes (pure, importable).
  - `gen_traces.py` — builds the Chakra trace library via stage.
- `tracelib/` — generated Chakra traces (gitignored; never committed).

Later phases (configs, calibration, sweep run, analysis) will be added as
additional helpers and `reproduce.sh` phases.

## Prerequisites

**Docker is required.** The `stage` toolchain that emits Chakra traces runs
only inside the `astra:latest` image. Build it once from the **repo root**. The
build clones `astra-sim/stage` internally (network access needed, no build args)
and requires Docker BuildKit/buildx — bundled with Docker >= 23.0; on older
installs add the plugin (`sudo apt-get install docker-buildx-plugin`) or use the
`DOCKER_BUILDKIT=1` fallback:

```bash
docker buildx build -t astra:latest .
# or, without the buildx plugin:
DOCKER_BUILDKIT=1 docker build -t astra:latest .
```

Override the docker command (e.g. rootless) and image via environment:
`DOCKER="docker"` and `STAGE_IMAGE=my-astra:tag`.

## Trace generation

`./reproduce.sh traces` builds one Chakra execution trace per **legal job
shape** for the bandwidth-bound model, under `tracelib/bw/<AxBxC>/`. A shape is
an ordered `A×B×C` where each dimension is independently **1 or an even integer
≤ 16** and `A·B·C ≤ 4096` (the torus capacity) — **729** shapes in total. Inside
each shape folder are the per-rank `chakra_trace.<rank>.et` files (one per rank,
`A·B·C` of them) plus a `chakra_trace.json`.

The build is **deterministic** (re-running yields the same traces for a fixed
`stage` version / image) and **idempotent / resumable** (shapes already built
are skipped). It runs `stage` in one batched container, fanning shapes out
across cores; parallelism is bounded by `JOBS` (default `nproc-2`).

Useful flags (call the helper directly):

```bash
# Build everything (≈729 shapes — large; tens of GB of .et files):
./reproduce.sh traces

# Plumbing test: build just 2x2x2 and verify the .et appears:
python3 scripts/gen_traces.py --smoke

# Build a specific subset:
python3 scripts/gen_traces.py --only 2x4x8,16x16x16

# List the legal shapes without building:
python3 scripts/shapes.py list bw
```

Output is gitignored wholesale — only the scripts are committed, so the traces
are reproduced from source rather than stored in git.
