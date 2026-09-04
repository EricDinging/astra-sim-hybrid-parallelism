# t3d

Sweep harness, configs, and results for the **3-D torus cluster**
experiments. The cluster dims (e.g. 16×16×16 = 4096 nodes) are asked once by
`./reproduce.py prereq` and saved to `cluster.json`; everything geometric —
legal job shapes, experiment max-job sizes, `--npus-per-dim`, the network
size in `configs/network.yml`, the launcher's memory envelope — derives from
it. Everything is driven from `reproduce.py`; every artifact (traces,
service times, results) is reproducible from source.

## Quick start

```bash
# 1. Install packages (Ubuntu): simulator build deps + Docker >= 23 for the
#    stage trace generator
sudo apt-get install cmake g++ libprotobuf-dev protobuf-compiler \
    libscotch-dev libhwloc-dev libjemalloc-dev libboost-dev rsync docker-buildx-plugin

# 2. Build the stage Docker image (from the REPO ROOT; needs network)
docker buildx build -t astra:latest .

# 3. Prerequisites: cluster dims (prompted once -> cluster.json), then
#    Chakra tracelib (~11 GB at 16x16x16) + measured service_times.csv
./reproduce.py prereq

# 4. Generate arrival traces (or run ./reproduce.py bare for a picker)
./reproduce.py gen <experiment[,experiment...]|all>

# 5. List your servers (user@host per line; empty/missing file = run locally),
#    then launch
echo you@server1.example.org >> workers.txt
./reproduce.py launch <experiment[,experiment...]|all>
```

There is no separate compile step: `prereq` and `launch` build the simulator
binary themselves via `build/astra_analytical/build.sh`. At launch the
workers are probed over SSH for their CPU features, and the build enables
the Haswell ISA floor (`-march=haswell`, ~15–20% faster sims, results
byte-identical) only when **every** usable worker — and this machine —
supports AVX2/BMI2/FMA; otherwise it falls back to a fully portable build
automatically. Rebuilds are incremental, so an unchanged tree costs seconds.
To build by hand anyway: `build/astra_analytical/build.sh` (`-a auto`
detects the local CPU, `-a generic` forces portable, `-p gen|use` are the
optional PGO phases — see `docs/superpowers/reports/2026-08-08-perf-round4.md`).

For quick dev tests use short traces: `N_JOBS=20 ./reproduce.py gen <exp>`.
`./reproduce.py clean` removes all results (keeps the prerequisites).

## Layout

- `reproduce.py` — driver. Bare run = interactive picker (option 0 = cleanup).
  Phases: `prereq`, `gen`, `launch`, `monitor`, `collect`, `clean` (soon: `post`).
- `scripts/` — the libraries behind the phases: `sweep.py` (experiment table +
  phase functions), `launcher.py` (worker probe/packing/deploy),
  `run_combo.py` (one sim on a worker), `shapes.py`, `gen_traces.py`,
  `measure_svc.py`, `gen_arrivals.py`, `make_jobs.py`.
- `configs/` — simulator configs. The generated BW/LT matrices are suffixed
  by topology: `*_schedule_torus.txt` (all real policies) and
  `*_schedule_fullmesh.txt` (the `ideal` placement).
- `tests/` — self-running tests, e.g. `python3 tests/test_sweep.py`.
- `tracelib/` — Chakra traces, one per legal shape (gitignored). Shared by all
  experiments; a job's iteration count lives in `arrivals.csv`, not the trace.
- `runs/<experiment>/<admission>-<placement>-load<L>/` — one flat folder per
  combo (7 placements × 20 loads, ρ = 0.05…1.00) holding its `arrivals.csv`
  and all results: streaming `progress.csv` + `occupancy.csv`, `jobs.csv`,
  `summary.txt`, `sim.done`. The `ideal` placement runs sfc on a fully
  connected mesh (`--fullmesh` + the `*_schedule_fullmesh.txt` matrices):
  placement and routing constraints vanish, giving the JCT baseline the six
  real policies are normalized against.

## Experiments

Six load sweeps over Poisson arrivals, n=60000 jobs, seed 0: admission
(easy/swf/fifo) × job-size distribution (Pareto α=0.5 capped at 512/256/128,
or uniform capped at 512). See `EXPERIMENTS` in `scripts/sweep.py`.

## Prerequisites (`./reproduce.py prereq`)

Builds the tracelib via stage-in-Docker (one trace per legal `A×B×C` shape:
dims 1 or even ≤ 16, ≤ 4096 ranks — 729 shapes; resumable, built shapes are
skipped) and measures each shape's isolated single-iteration JCT into
`service_times.csv` (shapes already in it are skipped; delete it to
re-measure everything). The `<nd>donly` sweeps' shape universe is probed
first (`rfold_placeable<cap>.txt`). Both the probe and the measure prompt
for a workers file (default `workers.txt`, empty = this machine) and run
one sim per worker slot with dynamic balancing, so a rerun after a
partial or interrupted measure only does the missing shapes. `clean` never
touches any of these. Env: `STAGE_IMAGE` (default `astra:latest`),
`DOCKER` (default `sudo docker`), `JOBS` (tracelib build parallelism,
default nproc−2).

## Trace generation (`./reproduce.py gen`)

For each combo, draws the job stream (sizes from the experiment's
distribution, shapes uniform per size, durations clamped log-normal) and lays
a Poisson arrival process calibrated to the combo's offered load ρ via the
measured service times, into `runs/<exp>/<combo>/arrivals.csv`. Deterministic
(seed-pinned): all placements at one load get identical arrivals. Skips
combos that already have arrivals. Full knob reference: docstring of
`scripts/gen_arrivals.py`.

## Launching (`./reproduce.py launch`)

Probes each worker for cpu count and available memory: a host runs at most
`min(cpus − 2, 85% of mem / envelope)` concurrent sims, with a measured
per-sim envelope of 3 GB at 8x8x8 (24 GB at 4096 NPUs). All requested
experiments' combos are packed jointly onto the fleet highest-load-first,
and the plan recorded in `runs/<exp>/assignments.csv`. Per host it rsyncs the bundle (binary,
exact-ABI libs, cluster.json, configs, scripts, assigned arrivals,
tracelib — first sync is the slow one) to `/workspace/cluster<capacity>`
(e.g. `/workspace/cluster4096`) and starts a detached
runner that works through its queue. Sims write results (including
per-job-flushed `progress.csv` and per-event `occupancy.csv`) straight into their
combo folder. Relaunch is safe: finished combos are skipped, stale twins are
killed.

## Syncing results back (`./reproduce.py collect`)

Sims write results on the workers; `collect` rsyncs them back into the local
`runs/<exp>/<combo>/` folders and prints a per-combo completion summary. It
reads `runs/<exp>/assignments.csv` to know which host holds each combo, pulls
from every host in parallel, then reports how many combos are done
(`sim.done` present) vs still pending.

```bash
./reproduce.py collect <experiment[,experiment...]|all>
```

Safe to run anytime — even mid-run: only result files come back
(`*.csv`, `sim.done`, `run.err`, `failed_nodes.log`), and `progress.csv` /
`occupancy.csv` are streamed, so partial progress shows up on each pull. Runs
are idempotent (rsync only moves changed files), so re-run it periodically to
refresh. The verbose per-combo logs (`log.log*`, `err.log*`, `jct.log*`, up
to ~100 MB each) stay on the worker for on-demand debugging and are never
pulled; arrivals and the shared `jobs-load*` symlink dirs never move either.

Use `./reproduce.py monitor <experiment[,experiment...]|all>` to watch progress without
pulling files — it polls the workers (hourly by default, `POLL_SECS`
overrides) and prints a live done/pending grid.

**Detach and reattach.** The runners are `setsid`-detached on the workers, so
they survive local disconnects. Once `launch` prints `launched N combos ...`,
you can Ctrl-C the driver at any time — the sweep keeps running remotely. Then
`collect` to pull partial results and `monitor` to reattach, in any order, as
often as you like. (Ctrl-C *before* that line may leave some hosts without a
runner started.)
