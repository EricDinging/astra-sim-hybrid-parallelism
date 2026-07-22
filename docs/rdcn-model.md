# RDCN model: TPU-cluster assumptions and what the simulator models

Agreed baseline for the RFold algorithm discussion (2026-07-21). Two layers:
the rules of the real machine (R1–R7), and what our simulator assumes on top
of them (S1–S3), with each divergence marked as either *revised* or *kept
deliberately as a research extension*.

## Part 1 — The real TPU-style cluster (R1–R7)

**R1 — Cubes are the atoms.** The cluster is a collection of N×N×N-chip
cubes (4×4×4 on TPUv4; N is generic here and is the simulator's
`--block-size`). Inside a cube, links are static electrical wiring — a fixed
3-D mesh that always exists and can never be rewired.

**R2 — Every inter-cube link is optical, and only surface ports exist.**
Each cube face is an N×N grid of chips, and every face chip's outward port
runs to an optical circuit switch (OCS). Interior chips have all six links
consumed by intra-cube wiring, so an interior chip can never talk off-cube
directly. There is no direct cube-to-cube cable; the familiar "big torus" is
just one particular switch configuration.

**R3 — Connection legality (corrected 2026-07-21).** Switches are grouped
per axis, and a circuit must join ports at the **same within-face position
(i,j)** on the **same axis**. Polarity rules differ by endpoint:

| endpoints | legal pairings at matching (i,j) |
|---|---|
| two **different** cubes | **any polarity**: +x↔−x *and* +x↔+x (and −x↔−x) |
| the **same** cube | **only** +x(i,j)↔−x(i,j) — the wrap that closes a one-cube-wide ring |

Cross-cube polarity freedom is what makes twisted/reflected torus
configurations possible, and it means a chain may enter one cube on its +
face and the next cube on *its* + face (tiles traversed in alternating
directions).

**R4 — No proximity.** All cubes are interchangeable; physical placement on
the datacenter floor means nothing. Any two distinct cubes are *potentially*
one OCS hop apart — one hop once a circuit is wired, not connected at all
until then. Uniform potential, not uniform standing connectivity.

**R5 — Ports are exclusive (the only scarcity).** Each face port holds
exactly one circuit at a time. Wiring it to one partner means it serves no
other. Distance costs nothing; port allocation is the entire resource
problem.

**R6 — Rings close for free.** A chain of cubes along an axis becomes a ring
by wiring the last cube back to the first; the wrap link is an ordinary
circuit, no more expensive than any other.

**R7 — Slices are cube-granular and isolated.** Real TPU jobs get slices
whose sides are multiples of N (whole cubes), wired into a private torus
(twists allowed). A job's traffic never rides another job's links; there is
no shared fabric between slices.

## Part 2 — Simulator assumptions (S1–S3)

**S1 — REVISED: the default torus is the initial OCS configuration, not
hardware.** The simulator's static all-machine torus is henceforth read as
"the circuits that happen to be wired at boot." Adjacent-cube seams are
*not* cheaper than remote seams — both are one circuit (R4). Because S3 is
kept, this reinterpretation changes no simulation behavior: the default
circuits are exactly the shared fabric multi-hop traffic rides. The revision is fully
implemented (2026-07-22): no ranking key prices circuits or default seams,
realizability accepts cross-cube same-polarity pairings per corrected R3
(`BlockModel`, gated on `ocs_enabled` — false only at whole-torus block,
where no OCS exists and folding-only semantics apply), and the
reconfiguration-count question dissolved when its only consumer (the
cost-model ranker) was deleted. Still unexploited: mirrored tile traversal
(flipping a tile's direction to dodge a busy port).

**S2 — KEPT deliberately: sub-cube placements.** Our jobs may have any
even-sided shape, occupy fractions of cubes, and share a cube — finer
granularity than R7 allows. This is a research extension, not an error: it
asks what a TPU-style cluster would gain if it supported sub-cube slices,
and what that costs.

**S3 — KEPT deliberately: a shared fabric.** Ring edges a placement cannot
wire ride multi-hop default routing over links other jobs also use —
impossible under R7 isolation. Same rationale: a deliberate extension to
study what shared-fabric support would take and be worth.

## Part 3 — Established consequences (evidence on record)

- **The contiguity tier was an artifact of primitive non-contiguous
  placement, now removed (2026-07-22).** The 2026-07-19 experiment that made
  "non-contiguous competes as an equal" look catastrophic (mean JCT 7.2 s →
  43–65 s) predated tile packing: origin-locked tiles sprayed fragments
  into idle cubes. With cube sharing, per-tile offsets, and the
  fragmented-cube-first order in place, dropping the tier *wins* every
  probe cell (5k jobs, mean JCT: fifo-0.60 −4.8 %, fifo-0.90 −15 %,
  easy-0.90 −2.5 %). The shipped ranking is tier-free:
  `(frag, cubes, multihop, cost)` for every placement.
- **The damage metric is newly-fragmented-cube count, not distance.**
  A placement pays only for cubes it takes from fully idle to partially
  occupied; consuming whole cubes or adding to already-fragmented ones is
  free. Any set of idle cubes is as good as any other.
- **Remaining conservatism.** Polarity-free pairings are legal in the
  model but no search yet exploits mirrored tile traversal.

## Part 4 — Change checklist (final, 2026-07-22)

- [x] Corrected-R3 realizability (cross-cube any polarity; same-cube
      opposite-face only), gated on `ocs_enabled` (false = whole-torus
      block, the folding-only arm)
- [x] R5 port exclusivity (`BlockModel::wirable_subset`) — found by the new
      RDCN legality checker; a wired ring closure can no longer share a
      face port with a default-circuit ride of the same placement
- [x] Best-effort wiring for non-contiguous placement: unwirable ring edges
      ride the shared fabric as multi-hop instead of vetoing the placement
      (the old veto restricted non-contiguous side lengths to {1, 2, 4, 8})
- [x] Tile packing with cube sharing: per-tile in-cube offsets, same- and
      cross-job cube sharing on disjoint chips, fragmented-cube-first order
- [x] Fragmentation-first ranking `(frag, cubes, multihop, cost)` shipped
      as the default; the contiguity tier removed after losing every probe
      cell (up to −15% mean JCT)
- [x] Single version: rfoldv1 and every superseded study arm (uniform-seams,
      fidelity-first, cost-model, max-defrag, ocs-priced scorer) deleted;
      `rfold` is the only policy name
- [x] Micro-benchmark layer: `tests/scheduling/rdcn_check.hh` legality
      checker + `microbench_rfold` scenarios (metrics in the ctest log)
- [x] Thesis prose (`docs/algo.tex`) updated to the final algorithm and
      agreed terminology
- [ ] Mirrored tile traversal (optional exploitation of polarity freedom)
- [ ] OPEN (user decision): EASY-admission fairness at full trace length —
      at 20k jobs the earlier (tiered) config traded small-job wait for
      big-job wait (mean +27% at easy-0.90); the tier-free ranking improved
      easy-0.90 in 5k-job probes, but a full-length re-validation is
      pending

### Validation (20k-job cells, mean JCT seconds, 0 drops everywhere)

| cell | folding (b8) | base b4 | +DOR-glue | +nesting (final) |
|---|---|---|---|---|
| p128 fifo 0.60 | 8.56 | 7.23 | 7.39 | **6.60** (−9%) |
| p128 fifo 0.74 | 120.5 | 106.9 | 76.8 | **55.4** (−48%) |
| p128 fifo 0.90 | 217.9 | 208.5 | 188.6 | **171.5** (−18%) |
| p128 easy 0.90 | 23.9 | 22.9 | 26.1 | 29.1 (+27%, fairness shift) |
| p256 fifo 0.60 | 360.9 | 348.7 | 277.4 | **49.7** (−86%) |
| p256 fifo 0.90 | 661.6 | 639.4 | 576.5 | **341.4** (−47%) |
