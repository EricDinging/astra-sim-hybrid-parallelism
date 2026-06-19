# Pareto-sized, load-calibrated 16×16×16 placement sweep (7 policies)

8 traces = (latency-bound | bandwidth-bound) × (light | heavy load) × (small |
large jobs). Each is a 120-job trace on the 16×16×16 = 4096-NPU torus, sizes
drawn truncated-Pareto (pmf ∝ s^-1.5) over an even-dims shape palette, seed 1.
All 7 placement policies (firstfit, random, sfc, l1clustering, topomatch,
folding, rfold) run per cell under fifo admission. Per-run results live in
flat folders under `../results/` (see `../results/README.md`).

Offered load is calibrated: ρ = Σ(size·svc)/(4096·span) with per-(model,shape)
isolated service times (`../calibration/service_times.csv`); light targets
0.15, heavy 0.85. A firstfit probe rescales ia until measured load is in band
(±0.10) — 10-job prefix for most cells, the full trace for `lat_heavy_large`
(see `reproduce.sh` SPEC comment).

The two heavy_large cells use curated size mixes (2026-06-10 retune) instead
of the full large grid: bulk fragmenters (512–672) shred contiguity while the
victims (1024/1536) are exactly the OCS-scatterable sizes, so block-level
rfold places immediately where whole-torus folding defers. Result: rfold
beats folding by ~21–26 % p50 JCT in those cells (vs ~2 % on the uncurated
grid) with every other policy ordering preserved.

`jobs/` symlink trees are gitignored; `reproduce.sh gen` (or the remote
farm runbook) regenerates them from `mixlib/` + the recorded per-cell ia in
`../calibration/cells.csv` — arrival files are bit-reproducible (seed 1).
