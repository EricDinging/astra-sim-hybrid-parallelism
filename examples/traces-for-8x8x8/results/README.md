# Flat per-run results

Every simulation run of the 8x8x8 sweep lives in one flat folder here, named

    <placement>-<admission>-<bw|lat>-<heavy|light>-<small|large>-<block>-<failprob>

Splitting a folder name on `-` always yields exactly 7 fields:

| field | values |
|---|---|
| placement | `firstfit` `random` `sfc` `l1clustering` `topomatch` `folding` `rfold` |
| admission | `fifo` `sjdf` `sjsf` `swf` `easy` `ljsf` `ljsfpack` `lwf` |
| trace: bound | `bw` (bandwidth-bound model) / `lat` (latency-bound model) |
| trace: load | `heavy` (rho 0.85) / `light` (rho 0.15) |
| trace: size | `small` (jobs 4–16 NPUs) / `large` (64–256 NPUs) |
| block | rfold `--block-size` in effect: `1x1x1` `2x2x2` `4x4x4` `8x8x8` |
| failprob | `--failure-prob`: `0` (none), `0.001`, `0.005`, `0.01` |

Each folder holds `jobs.csv`, `node_jobs.csv`, `summary.txt` and, for
failure runs, the `failed_nodes.log` manifest. Console output (`run.log`)
is gitignored and deleted after runs; a fresh run recreates it locally.
The trace inputs are in `../traces/<cell>/`.

## Block-size conventions

- The binary's default `--block-size` is **4x4x4**, so every run that did
  not sweep the block carries the `4x4x4` label. For the five placements
  that never reconfigure (`firstfit` `random` `sfc` `l1clustering`
  `topomatch`) the knob is inert — the label just records what was in
  effect.
- **`folding` is rfold pinned to the whole torus** (`--block-size=8x8x8`,
  OCS structurally impossible), so folding folders always carry `8x8x8`.
  There is deliberately **no `rfold-...-8x8x8-...` folder**: that run is
  byte-identical to the `folding-...-8x8x8-...` one. A block-size sweep
  over rfold therefore reads `1x1x1`/`2x2x2` from `rfold-*`, `4x4x4` from
  the baseline `rfold-*` run, and the `8x8x8` point from `folding-*`.

## Coverage

- Baseline grid: 7 placements x 8 admissions x 8 traces at default block,
  failprob 0 (448 folders). The 8th admission, `ljsfpack` (non-blocking FFD:
  `ljsf` size ordering but skips unplaceable jobs instead of head-of-line
  blocking), lives in this baseline grid only — like the other size-only
  policies it has no block/failure variants.
- Block sweep: `rfold` x {`fifo`,`easy`} x 8 traces at blocks `1x1x1` and
  `2x2x2` (32 folders; `4x4x4`/`8x8x8` points are the baseline folders, see
  above).
- Failure sweep: 7 placements x {`fifo`,`easy`} x {`bw-heavy-large`,
  `lat-light-small`} x failprob {`0.001`,`0.005`,`0.01`} (84 folders).

Runs are produced by `../reproduce.sh` (baseline) and
`../scripts/sweep_block.sh` / `../scripts/sweep_failure.sh`; all three are
idempotent against these folders (a run with an existing `jobs.csv` is
skipped).

## placeability/

`placeability/` is a self-contained companion experiment (JCR on a trace of
irregular/oversized job shapes, firstfit vs rfold) and does not follow the
naming scheme — see `placeability/README.md`. Plotting code that globs this
directory should skip it.
