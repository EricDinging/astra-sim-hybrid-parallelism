# Flat per-run results

Every simulation run of the 16x16x16 sweep lives in one flat folder here,
named

    <placement>-<admission>-<bw|lat>-<heavy|light>-<small|large>-<block>-<failprob>

Splitting a folder name on `-` always yields exactly 7 fields:

| field | values |
|---|---|
| placement | `firstfit` `random` `sfc` `l1clustering` `topomatch` `folding` `rfold` |
| admission | `fifo` `sjdf` `sjsf` `swf` `easy` `ljdf` `ljsf` `ljsfpack` `lwf` |
| trace: bound | `bw` (bandwidth-bound model) / `lat` (latency-bound model) |
| trace: load | `heavy` (rho 0.85) / `light` (rho 0.15) |
| trace: size | `small` (jobs 8–256 NPUs) / `large` (512–2048; heavy cells use curated mixes) |
| block | rfold `--block-size` in effect: `4x4x4` (binary default), `16x16x16` (folding pins the whole torus) |
| failprob | `--failure-prob`: `0` (none) |

Each folder holds `jobs.csv`, `node_jobs.csv`, `summary.txt`. Console output
(`run.log`) is gitignored and deleted after runs. The trace inputs are in
`../traces/<cell>/`.

Provenance: the two `*-heavy-large-*` cells were re-run 2026-06-11 on the
retuned traces; the other six cells carry the 2026-06-08 sweep results and
are due a refresh under the unified folding/rfold binary (known follow-up).

Admission `ljsfpack` (non-blocking FFD: `ljsf` size ordering but skips
unplaceable jobs instead of head-of-line blocking) was swept across all 7
placements × 8 cells on the farm (2026-06-18) — placement grid only, no
block/failure variants.
