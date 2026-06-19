# Pareto-sized, load-calibrated 2×2×2 placement sweep (7 policies)

8 traces = (latency-bound | bandwidth-bound) × (light | heavy load) × (small |
large jobs). Each is a 400-job trace on the 8×8×8 = 512-NPU torus. All 7
placement policies (firstfit, random, sfc, l1clustering, topomatch, folding,
rfold) ran on the CloudLab farm. **rfold (repo defaults) wins every
cell on p50 JCT and throughput.** Per-run results live in flat folders under
`../results/` (see `../results/README.md` for the naming scheme).

Replaces the earlier hand-tuned sweep (archived at
`examples/traces-for-8x8x8.old-20260605/`). Three things changed:

1. **Offered load is calibrated, not guessed.** ρ = Σ(size·svc)/(512·span),
   with per-(model,shape) isolated service times measured by single-job sims
   (`../calibration/service_times.csv`). Light targets 0.15, heavy 0.85; after
   the runs, measured load (firstfit jct−queue_wait) is verified in band
   [target±0.05] and the cell is rescaled + rerun if not (`reproduce.sh`
   verify loop — fired once, for lat_heavy_small).
2. **Job sizes are truncated-Pareto**: small cells sample {4,8,16} NPUs, large
   cells {64,128,256}, pmf ∝ 1/size (57.1/28.6/14.3 % expected; seed-1
   realizations 59.8/24.5/15.8 small, 63.0/25.5/11.5 large).
3. **Shapes are uniform over ALL factorizations**: a size-S job picks uniformly
   among every ordered A×B×C with A·B·C=S, dims ≤ 8 (47 shapes per model,
   `scripts/palette.py`). Shape maps to STG dp×tp×pp, so each is a distinct
   trace (94-entry `../mixlib/`).

The lat/bw STG models are byte-identical to the old sweep (lat: dmodel 1024
dff 2048 batch 1 seq 256; bw: dmodel 8192 dff 16384 batch 16 seq 2048).

## Offered-load verification

| cell | target | isolated | measured | in band | size mix % |
|---|---|---|---|---|---|
| lat_light_small | 0.15 | 0.15 | 0.178 | yes | 59.8/24.5/15.8 |
| lat_light_large | 0.15 | 0.15 | 0.151 | yes | 63.0/25.5/11.5 |
| lat_heavy_small | 0.85 | 0.69* | 0.864 | yes | 59.8/24.5/15.8 |
| lat_heavy_large | 0.85 | 0.85 | 0.877 | yes | 63.0/25.5/11.5 |
| bw_light_small | 0.15 | 0.15 | 0.152 | yes | 59.8/24.5/15.8 |
| bw_light_large | 0.15 | 0.15 | 0.150 | yes | 63.0/25.5/11.5 |
| bw_heavy_small | 0.85 | 0.85 | 0.863 | yes | 59.8/24.5/15.8 |
| bw_heavy_large | 0.85 | 0.85 | 0.862 | yes | 63.0/25.5/11.5 |

\* lat_heavy_small needed one verify-loop rescale: dense packing of tiny
latency-bound jobs inflates measured service ~22% over isolated, so its
arrivals were widened (ia 38251→46810 ns), trading isolated load for
on-target measured load.

## Headline: rfold sweeps all 8 cells

p50 JCT relative to firstfit (lower = better); makespan ratio in parens:

| cell | rfold/ff p50 | rfold/ff p99 | folding/ff p50 | random/ff p50 | rfold/ff makespan |
|---|---|---|---|---|---|
| lat_light_small | **0.60** | 0.97 | 1.31 | 2.62 | 0.99 |
| lat_light_large | **0.98** | 0.99 | 1.94 | 4.21 | 1.00 |
| lat_heavy_small | **0.45** | 0.78 | 3.00 | 7.62 | 0.92 |
| lat_heavy_large | **0.28** | 0.39 | 4.17 | 11.17 | 0.86 |
| bw_light_small | **0.85** | 0.89 | 1.29 | 1.37 | 1.00 |
| bw_light_large | **0.89** | 0.96 | 1.46 | 2.20 | 1.00 |
| bw_heavy_small | **0.64** | 0.71 | 1.23 | 2.20 | 0.87 |
| bw_heavy_large | **0.21** | 0.26 | 2.00 | 4.52 | 0.82 |

- rfold's edge **grows with load and job size** (heavy·large: 3.5–4.8× faster
  p50 than firstfit, 14–18% shorter makespan) — reconfiguration pays most
  when contiguous space is scarce.
- The scatter policies (random/sfc/l1clustering/topomatch) trail firstfit
  everywhere; the gap peaks at lat_heavy_large (11× p50) where hop count
  dominates tiny messages.
- folding places more jobs but pays a comm penalty on the static torus
  (1.2–4.2× firstfit p50), consistent with the earlier folding study.

## Simulator fix this sweep depends on

The new palette includes pure-DP shapes (e.g. 8x1x1) that put >2 concurrently
dep-free collectives on one comm group. With
`HardwareResource::kMaxInflightGpuCommOps = 2`, ranks under scattered
placement admitted divergent 2-subsets and circular-waited (event queue
drained with jobs incomplete; 5 policies × both bw_small cells). Fixed by a
liveness valve in `SchedRuntime::run()`: at quiescence with jobs incomplete,
re-issue dep-free nodes once with the cap lifted. Runs that never deadlock
are byte-identical (verified: firstfit/rfold outputs unchanged pre/post fix);
previously-deadlocked sims complete with the valve firing exactly once.

## Reproduce

No trace data is shipped; everything regenerates deterministically (seed 1):

    cd examples/traces-for-8x8x8
    ./reproduce.sh lib        # STG trace library via Docker astra:latest (94 entries)
    ./reproduce.sh calibrate  # 94 isolated service times (local sims)
    ./reproduce.sh gen        # exact per-cell mean-ia from rho targets + gates
    ./reproduce.sh run        # farm run (sshlist hosts) + load verify loop

Each cell keeps `arrivals.csv`, `job_models.csv`, `trace_config.txt` (and a
regenerable `jobs/` dir, gitignored); per-run results live in `../results/`
and per-cell ia / calibration artifacts in `../calibration/`.
