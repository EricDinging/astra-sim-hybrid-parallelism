# cluster4096

Sweep harness, configs, and results for the **4096-node cluster** experiments
(16×16×16 3-D torus). Everything is driven from `reproduce.py`; every
artifact (traces, service times, results) is reproducible from source.

## Quick start

```bash
# 1. Install packages (Ubuntu): simulator build deps + Docker >= 23 for the
#    stage trace generator
sudo apt-get install cmake g++ libprotobuf-dev protobuf-compiler \
    libscotch-dev libhwloc-dev libjemalloc-dev libboost-dev rsync docker-buildx-plugin

# 2. Build the stage Docker image (from the REPO ROOT; needs network)
docker buildx build -t astra:latest .

# 3. Build the simulator binary
build/astra_analytical/build.sh

# 4. Prerequisites: Chakra tracelib (~11 GB) + measured service_times.csv
./reproduce.py prereq

# 5. Generate arrival traces (or run ./reproduce.py bare for a picker)
./reproduce.py gen <experiment|all>

# 6. List your servers (user@host per line; empty/missing file = run locally),
#    then launch
echo you@server1.example.org >> workers.txt
./reproduce.py launch <experiment|all>
```

For quick dev tests use short traces: `N_JOBS=20 ./reproduce.py gen <exp>`.
`./reproduce.py clean` removes all results (keeps the prerequisites).

## Layout

- `reproduce.py` — driver. Bare run = interactive picker (option 0 = cleanup).
  Phases: `prereq`, `gen`, `launch`, `clean` (soon: `collect`, `post`).
- `scripts/` — the libraries behind the phases: `sweep.py` (experiment table +
  phase functions), `launcher.py` (worker probe/packing/deploy),
  `run_combo.py` (one sim on a worker), `shapes.py`, `gen_traces.py`,
  `measure_svc.py`, `gen_arrivals.py`, `make_jobs.py`.
- `configs/` — 16³-torus simulator configs.
- `tests/` — self-running tests, e.g. `python3 tests/test_sweep.py`.
- `tracelib/` — Chakra traces, one per legal shape (gitignored). Shared by all
  experiments; a job's iteration count lives in `arrivals.csv`, not the trace.
- `runs/<experiment>/<admission>-<placement>-load<L>/` — one flat folder per
  combo (6 placements × 20 loads, ρ = 0.05…1.00) holding its `arrivals.csv`
  and all results: streaming `jct.csv` + `occupancy.csv`, `jobs.csv`,
  `summary.txt`, `sim.done`.

## Experiments

Six load sweeps over Poisson arrivals, n=100000 jobs, seed 0: admission
(easy/swf/fifo) × job-size distribution (Pareto α=0.5 capped at 512/256/128,
or uniform capped at 512). See `EXPERIMENTS` in `scripts/sweep.py`.

## Prerequisites (`./reproduce.py prereq`)

Builds the tracelib via stage-in-Docker (one trace per legal `A×B×C` shape:
dims 1 or even ≤ 16, ≤ 4096 ranks — 729 shapes; resumable, built shapes are
skipped) and measures each shape's isolated single-iteration JCT into
`service_times.csv` (skipped when present; delete it to re-measure).
`clean` never touches either. Env: `STAGE_IMAGE` (default `astra:latest`),
`DOCKER` (default `sudo docker`), `JOBS` (parallelism, default nproc−2).

## Trace generation (`./reproduce.py gen`)

For each combo, draws the job stream (sizes from the experiment's
distribution, shapes uniform per size, durations clamped log-normal) and lays
a Poisson arrival process calibrated to the combo's offered load ρ via the
measured service times, into `runs/<exp>/<combo>/arrivals.csv`. Deterministic
(seed-pinned): all placements at one load get identical arrivals. Skips
combos that already have arrivals. Full knob reference: docstring of
`scripts/gen_arrivals.py`.

## Launching (`./reproduce.py launch`)

Probes each worker (cpu count + available memory; a host runs at most
`min(cpus − 2, mem / 24 GB)` concurrent sims), packs all requested
experiments' combos jointly onto the fleet highest-load-first, and records
the plan in `runs/<exp>/assignments.csv`. Per host it rsyncs the bundle
(binary, exact-ABI libs, configs, scripts, assigned arrivals, tracelib —
first sync is the slow one) to `/workspace/cluster4096` and starts a detached
runner that works through its queue. Sims write results (including
per-job-flushed `jct.csv` and per-event `occupancy.csv`) straight into their
combo folder. Relaunch is safe: finished combos are skipped, stale twins are
killed.
