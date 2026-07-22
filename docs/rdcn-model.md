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
circuits are exactly the shared fabric multi-hop traffic rides. What must
change to finish the revision:

1. *Ranking:* drop the `ocs_links` tiebreak from the default (rfoldv2)
   rank key — the last remnant of "prefer default seams." Key becomes
   `(class, multihop, cubes, cost)`. Needs the standard 4-cell validation
   before shipping (deep tiebreak, near-zero effect expected, unverified).
   The `rfoldv1` bit-compat arm keeps the old key.
2. *Realizability:* accept cross-cube same-polarity pairings per corrected
   R3. Today `BlockModel::ocs_axis` / `ocs_realizable` require opposite
   faces — **stricter than the real machine**, so current results are
   conservative (every accepted placement is realizable; some realizable
   placements are rejected). Exploiting the freedom (mirrored tile
   traversal, so a busy port can be dodged by flipping a tile's direction)
   is a follow-on extension.
3. *Open decision:* what "reconfiguration count" measures — every circuit a
   job wires (uniform, TPU-faithful) vs. only circuits moved away from the
   current configuration (what rewiring downtime actually costs). Leaning
   toward the second, since switching time is the only physical cost that
   survives R4.

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

- **The class gate is structural, not adjacency-based.** Removing it
  (uniform-seams experiment, 8×20k-job runs, 2026-07-19): mean JCT went
  from 7.2 s to 43–65 s at fifo/0.60, with eager gluing rising only
  0.8 % → 3 %. The gate protects *clean cubes* from slivers, a mechanism
  that exists below cube granularity where even real TPUs have static
  wiring. It survives the S1 revision untouched.
- **The damage metric is dirty-cube count, not distance.** An awkward shape
  (side not divisible by N) must leave some cube partly full; eager
  scattering dirties several cubes per job (tiles pin to cube origins, so
  leftovers cannot be shared with the next job), starving future jobs of
  clean cubes. Whole-cube placements dirty nothing and are unconditionally
  fine — any set of free cubes is as good as any other.
- **Model conservatism.** Until S1-revision item 2 lands, the simulator
  under-uses the real machine's wiring freedom (opposite-face-only seams).

## Part 4 — Change checklist (updated 2026-07-22)

- [x] Drop `ocs_links` from the rfoldv2 rank key (validated, 2 rounds)
- [x] Relax `ocs_axis`/`ocs_realizable` to corrected R3 (cross-cube any
      polarity; same-cube opposite-face only) — `BlockModel` polarity_free
      flag; v1 strict for bit-compat
- [x] **(found by the new RDCN checker)** R5 port-exclusivity fix: a wired
      ring closure can no longer share a face port with a default-seam ride
      of the same placement (`BlockModel::wirable_subset`)
- [x] **DOR-tolerant glue** (major addition): the scatter gate no longer
      rejects variants with unwirable edges — they ride the S3 shared
      fabric as `dor_edges`. Root cause fixed: glueable dims were
      {1, 2, 4, 8}, so every factor-3 shape (the 72-chip family, ~10% of
      pareto128 chip-mass) could never glue at all
- [x] **Tile nesting** (`scatter_assign_nested`): per-tile in-cube offsets,
      same/cross-job cube sharing on disjoint chips, dirty-cube-first
      candidate order; `dirty_delta` (newly-wounded clean cubes) leads the
      v2 scatter ranking. Whole-torus blocks (folding-only arm) keep strict
      pre-RDCN semantics — no OCS, no RDCN machinery
- [x] Micro-benchmark layer: `tests/scheduling/rdcn_check.hh` legality
      checker + `microbench_rfold` scenarios (metrics in the ctest log)
- [ ] Decide the reconfiguration-count definition (item S1.3)
- [ ] Mirrored tile traversal (optional exploitation of polarity freedom)
- [ ] Update thesis prose (`docs/algo.tex`) to the RDCN model
- [ ] OPEN (user decision): EASY-admission fairness tradeoff — gluing
      places big jobs earlier (big-job wait −15%) at small jobs' expense
      (mean JCT +27% at easy-0.90); options: accept, or queue-aware glue
      gating

### Validation (20k-job cells, mean JCT seconds, 0 drops everywhere)

| cell | folding (b8) | base b4 | +DOR-glue | +nesting (final) |
|---|---|---|---|---|
| p128 fifo 0.60 | 8.56 | 7.23 | 7.39 | **6.60** (−9%) |
| p128 fifo 0.74 | 120.5 | 106.9 | 76.8 | **55.4** (−48%) |
| p128 fifo 0.90 | 217.9 | 208.5 | 188.6 | **171.5** (−18%) |
| p128 easy 0.90 | 23.9 | 22.9 | 26.1 | 29.1 (+27%, fairness shift) |
| p256 fifo 0.60 | 360.9 | 348.7 | 277.4 | **49.7** (−86%) |
| p256 fifo 0.90 | 661.6 | 639.4 | 576.5 | **341.4** (−47%) |
