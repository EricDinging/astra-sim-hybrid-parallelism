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
  - `measure_svc.py` — measures the isolated single-iteration service time of
    each shape → `service_times.csv`.
  - `gen_arrivals.py` — generates an arrival-process trace (`arrivals.csv`) for
    a target offered load.
- `configs/` — 16³-torus simulator configs (system, network, BW/LT schedules)
  consumed by the binary.
- `tests/` — self-running unit tests for the `scripts/` helpers. Run any one
  directly, e.g. `python3 tests/test_shapes.py` (no pytest required).
- `tracelib/` — generated Chakra traces (gitignored; never committed).

Later phases (calibration wiring, sweep run, analysis) will be added as
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

## Arrival-process traces

Once the trace library exists, you build an **arrival trace** (`arrivals.csv`) in
two steps: measure each shape's service time, then sample a job stream at a
target offered load.

### Step 1 — measure service times → `service_times.csv`

`measure_svc.py` runs every shape's trace **alone** on the idle 16³ torus and
records its single-iteration JCT as that shape's `svc_per_iter_ns`. This needs
the built reconfigurable binary
(`build/astra_analytical/build/bin/AstraSim_Analytical_Reconfigurable`; see the
repo `CLAUDE.md` for `build.sh`) and the trace library from above.

```bash
python3 scripts/measure_svc.py \
    --traces tracelib \
    --cfg configs \
    --astra-sim ../../build/astra_analytical/build/bin/AstraSim_Analytical_Reconfigurable \
    --out service_times.csv
# restrict to a subset while testing:
python3 scripts/measure_svc.py --traces tracelib --cfg configs \
    --astra-sim <binary> --out svc_small.csv --only 2x2x2,4x4x4
```

Output `service_times.csv` has columns `shape,size,svc_per_iter_ns`, one row per
measured shape (`--jobs` bounds parallel sims, default `nproc-2`).

### Step 2 — generate an arrival trace → `arrivals.csv`

`gen_arrivals.py` draws a job stream and lays a **Poisson** arrival process
calibrated to a target offered load `ρ` (`--rho`). Each job's size is drawn from
a truncated Pareto over the legal sizes; its shape is uniform among shapes of
that size; its duration is `num_iterations` (currently a fixed placeholder of
1). Offered load is `ρ = W / (C·T)` with `W = Σ num_ranks·svc_per_iter·N`,
`C = 4096`; the script inverts `ρ` to a mean inter-arrival time. (See the module
docstring in `gen_arrivals.py` for the full load model.)

```bash
# 500-job trace at 80% offered load, using the measured table:
python3 scripts/gen_arrivals.py \
    --rho 0.8 --n 500 \
    --svc service_times.csv \
    --traces tracelib \
    --seed 0 \
    --out runs/load80

# Dev/smoke without a measured table — substitute one constant svc for all shapes:
python3 scripts/gen_arrivals.py --rho 0.5 --n 50 --uniform-svc-ns 1000 --out /tmp/cell
```

Exactly one of `--svc <table>` or `--uniform-svc-ns <ns>` is required. Useful
flags: `--alpha` (Pareto exponent, default `0.5`; lower = heavier large-job
tail), `--size-min`/`--size-max` (restrict drawn sizes), `--seed`
(reproducible), `--traces` (also create `jobs/<id>` symlinks into the library so
the trace is directly runnable by the simulator).

The output directory contains:

- `arrivals.csv` — `job_id,arrival_time_ns,num_ranks,shape,num_iterations`.
- `trace_config.txt` — the full argument dump plus the realized `W` and
  `mean_ia_ns` for provenance.
- `jobs/<id>` — symlinks to each job's trace dir (only when `--traces` is given).

Runs are **deterministic**: the same `--seed` (and inputs) reproduces the same
`arrivals.csv` byte-for-byte.
