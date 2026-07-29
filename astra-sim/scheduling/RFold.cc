/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/RFold.hh"

#include "astra-sim/common/Logging.hh"
#include "astra-sim/scheduling/BlockModel.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/FoldEnumerator.hh"
#include "astra-sim/scheduling/FootprintRouter.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AstraSim {
namespace Scheduling {

namespace {
// A rotation of footprint `fp` fits `dims` iff sorted(fp) <= sorted(dims).
bool fits_cluster(std::array<int, 3> fp, const std::vector<int>& dims) {
    std::array<int, 3> d{dims[0], dims[1], dims[2]};
    std::sort(fp.begin(), fp.end());
    std::sort(d.begin(), d.end());
    return fp[0] <= d[0] && fp[1] <= d[1] && fp[2] <= d[2];
}

// 1-D snake DFS bound (folding's placement-time budget); shared by the
// placement-of-last-resort snake and the idle-cluster oracle.
constexpr int kSnakeBudget = 200000;

// Node-visit budget for one round of brick-split candidate generation
// (all specs together); bounds the anchor scans at large torus sizes.
constexpr int kBrickBudget = 500000;

// DOR hop count between two global ids. Unidirectional fabric: always the
// +1 arc. Bidi fabric (--bidi): the backend routes the shorter arc per
// dimension, so pricing must too — charging the long way would overstate a
// relaxed placement's link load ~2x and misapply the stretch cap.
int dor_hops(int a, int b, const std::vector<int>& dims, bool bidi) {
    const auto ca = coord_of(a, dims[0], dims[1]);
    const auto cb = coord_of(b, dims[0], dims[1]);
    int h = 0;
    for (int d = 0; d < 3; ++d) {
        const int fwd = (cb[d] - ca[d] + dims[d]) % dims[d];
        h += bidi ? std::min(fwd, dims[d] - fwd) : fwd;
    }
    return h;
}

// A relaxed placement candidate after reconfiguration-aware finalization:
// adjacent ring edges pinned, OCS-realizable seams wired within port limits
// (R5), and the residual DOR edges priced as link load.
struct RelaxedCandidate {
    std::vector<int> rank_map;
    ReconfigPlan plan;
    int link_load = 0;
    // Ring edges left riding shared-fabric DOR (unpinned, unwired); with
    // link_load gives the candidate's average stretch = load / edges.
    int dor_edges = 0;
};

// Treat reconfiguration as first-class for ANY relaxed rank_map (design
// 4.1): pin a 1-hop route for every torus-adjacent ring edge, wire every
// non-adjacent edge the OCS can realize (scatter rules; port-checked via
// wirable_subset), and charge only the residual unwired edges' DOR hops.
RelaxedCandidate finalize_relaxed(std::vector<int> rank_map,
                                  const std::vector<std::pair<int, int>>& ring,
                                  const BlockModel& cm,
                                  const std::vector<int>& dims,
                                  bool bidi) {
    std::vector<std::pair<int, int>> phys;
    phys.reserve(ring.size());
    for (const auto& e : ring) {
        phys.emplace_back(rank_map[e.first], rank_map[e.second]);
    }
    std::vector<std::pair<int, int>> wires;
    std::set<std::pair<int, int>> seen;
    for (const auto& e : phys) {
        if (cm.is_torus_adjacent(e.first, e.second)) {
            continue;
        }
        const std::pair<int, int> key{std::min(e.first, e.second),
                                      std::max(e.first, e.second)};
        if (!seen.insert(key).second) {
            continue;
        }
        if (cm.ocs_enabled() &&
            cm.ocs_realizable_scatter(key.first, key.second)) {
            wires.push_back(key);
        }
    }
    RelaxedCandidate rc;
    rc.plan.ocs_edges = cm.wirable_subset(phys, wires);
    const std::set<std::pair<int, int>> acc(rc.plan.ocs_edges.begin(),
                                            rc.plan.ocs_edges.end());
    for (const auto& e : phys) {
        if (cm.is_torus_adjacent(e.first, e.second)) {
            continue;
        }
        const std::pair<int, int> key{std::min(e.first, e.second),
                                      std::max(e.first, e.second)};
        if (acc.count(key) > 0) {
            continue;
        }
        rc.link_load += dor_hops(e.first, e.second, dims, bidi);
        ++rc.dor_edges;
    }
    rc.rank_map = std::move(rank_map);
    rc.plan.routes = FootprintRouter::adjacent_routes(ring, rc.rank_map, dims,
                                                      rc.plan.ocs_edges);
    return rc;
}

// Brick-split placement: cut the job shape into k contiguous segments along
// one axis and place each segment as an axis-aligned slab. All segments
// share their anchor on the two perpendicular axes, so a seam edge differs
// from its partner only along the split axis — OCS-realizable whenever it
// lands on block faces (the aligned-slab constraint is what makes seams
// wirable at all; see design 4.1). Returns the rank_map or empty.
//
// Anchor scan order favors block-aligned positions first (face-aligned cuts
// are the wirable ones), then falls back to every position. First fit;
// `budget` counts node visits and aborts the scan when exhausted.
std::vector<int> place_bricks(const std::array<int, 3>& shape,
                              int axis,
                              int k,
                              const std::unordered_set<int>& free,
                              const std::vector<int>& dims,
                              const std::array<int, 3>& block,
                              int& budget) {
    const int seg_len = shape[axis] / k;
    const int a1 = (axis + 1) % 3;
    const int a2 = (axis + 2) % 3;
    // Candidate positions along one torus axis: block-aligned first, then
    // the rest.
    auto positions = [&](int ax) {
        std::vector<int> out;
        out.reserve(dims[ax]);
        for (int p = 0; p < dims[ax]; p += block[ax]) {
            out.push_back(p);
        }
        for (int p = 0; p < dims[ax]; ++p) {
            if (p % block[ax] != 0) {
                out.push_back(p);
            }
        }
        return out;
    };
    const auto pos_axis = positions(axis);
    const auto pos_a1 = positions(a1);
    const auto pos_a2 = positions(a2);

    // One segment's slab test-and-mark: segment s covers shape coords
    // [s*seg_len, (s+1)*seg_len) on `axis`, full extent on a1/a2.
    std::unordered_set<int> taken;
    auto try_segment = [&](int s, int x_seg, int p1, int p2,
                           bool mark) -> bool {
        for (int ca = 0; ca < seg_len && budget > 0; ++ca) {
            for (int c1 = 0; c1 < shape[a1]; ++c1) {
                for (int c2 = 0; c2 < shape[a2]; ++c2) {
                    --budget;
                    std::array<int, 3> t{};
                    t[axis] = (x_seg + ca) % dims[axis];
                    t[a1] = (p1 + c1) % dims[a1];
                    t[a2] = (p2 + c2) % dims[a2];
                    const int id =
                        t[2] * dims[1] * dims[0] + t[1] * dims[0] + t[0];
                    if (free.count(id) == 0 || (!mark && taken.count(id) > 0)) {
                        return false;
                    }
                    if (mark) {
                        taken.insert(id);
                    }
                }
            }
        }
        return budget > 0;
    };

    for (int p1 : pos_a1) {
        for (int p2 : pos_a2) {
            if (budget <= 0) {
                return {};
            }
            // Greedy: pick each segment's split-axis position in order.
            std::vector<int> seg_x(k, -1);
            taken.clear();
            bool ok = true;
            for (int s = 0; s < k && ok; ++s) {
                ok = false;
                for (int x : pos_axis) {
                    if (try_segment(s, x, p1, p2, false)) {
                        if (!try_segment(s, x, p1, p2, true)) {
                            return {};  // budget died mid-mark: abandon
                        }
                        seg_x[s] = x;
                        ok = true;
                        break;
                    }
                    if (budget <= 0) {
                        return {};
                    }
                }
            }
            if (!ok) {
                continue;
            }
            // Build the rank_map: DP-fastest linearization
            // i = pp*(s0*s1) + tp*s0 + dp, shape axes mapped straight onto
            // torus axes (no rotation).
            const int K = shape[0] * shape[1] * shape[2];
            std::vector<int> rank_map(K);
            for (int i = 0; i < K; ++i) {
                std::array<int, 3> c{i % shape[0], (i / shape[0]) % shape[1],
                                     i / (shape[0] * shape[1])};
                const int s = c[axis] / seg_len;
                std::array<int, 3> t{};
                t[axis] = (seg_x[s] + (c[axis] - s * seg_len)) % dims[axis];
                t[a1] = (p1 + c[a1]) % dims[a1];
                t[a2] = (p2 + c[a2]) % dims[a2];
                rank_map[i] = t[2] * dims[1] * dims[0] + t[1] * dims[0] + t[0];
            }
            return rank_map;
        }
    }
    return {};
}

// ---- Wired-only feasibility probe (diagnostic; design direction 1) --------
// Active only when RFOLD_WIRED_PROBE=<csv path> is set: at every long-wait
// fold-DEFER the expanded generator below runs and one CSV line records
// whether a zero-link-load (fully wired) candidate exists. Log-only; the
// DEFER outcome never changes, so probe runs stay trajectory-identical.

// Generator v2: cut the shape into a cuts[0] x cuts[1] x cuts[2] grid of
// bricks and place every brick independently at block-aligned anchors
// (greedy nearest-to-previous-brick, backtracking over the first brick's
// anchor). Cut extents must be block multiples, so seam and cut-axis closure
// edges join block faces at matching intra-block positions -- OCS-wirable by
// construction (ocs_axis matches perpendicular coords mod block; whole-block
// perpendicular shifts between bricks stay wirable, which is the freedom
// place_bricks's shared-anchor constraint gives up). Uncut axes must close
// by adjacency (extent 1, 2 or dims) or on faces (extent a block multiple),
// else no L=0 candidate can exist and the spec is skipped.
std::vector<int> place_bricks_v2(const std::array<int, 3>& shape,
                                 const std::array<int, 3>& cuts,
                                 const std::unordered_set<int>& free,
                                 const std::vector<int>& dims,
                                 const std::array<int, 3>& block,
                                 int& budget) {
    std::array<int, 3> ext{};
    for (int d = 0; d < 3; ++d) {
        if (cuts[d] < 1 || shape[d] % cuts[d] != 0) {
            return {};
        }
        ext[d] = shape[d] / cuts[d];
        if (ext[d] > dims[d]) {
            return {};  // a brick that wraps onto itself never fits
        }
        if (cuts[d] > 1) {
            if (ext[d] % block[d] != 0) {
                return {};
            }
        } else if (shape[d] > 2 && shape[d] != dims[d] &&
                   shape[d] % block[d] != 0) {
            return {};  // closure lands off-face: unwirable, never L=0
        }
    }
    // Anchor positions per axis: face-closure axes (cut axes, and uncut axes
    // closing on block faces) must be block-aligned; adjacency-closure axes
    // (extent <= 2 or == dims) are unconstrained -- wirability there only
    // needs congruent offsets mod block ACROSS bricks, enforced against
    // brick 0 in the greedy loop below.
    std::array<std::vector<int>, 3> pos;
    for (int d = 0; d < 3; ++d) {
        const bool free_anchor =
            cuts[d] == 1 && (ext[d] <= 2 || ext[d] == dims[d]);
        for (int p = 0; p < dims[d]; p += free_anchor ? 1 : block[d]) {
            pos[d].push_back(p);
        }
    }
    std::vector<std::array<int, 3>> anchors;
    for (int z : pos[2]) {
        for (int y : pos[1]) {
            for (int x : pos[0]) {
                anchors.push_back({x, y, z});
            }
        }
    }
    const int nb = cuts[0] * cuts[1] * cuts[2];
    std::unordered_set<int> taken;
    auto brick_fits = [&](const std::array<int, 3>& a, bool mark) -> bool {
        for (int c2 = 0; c2 < ext[2]; ++c2) {
            for (int c1 = 0; c1 < ext[1]; ++c1) {
                for (int c0 = 0; c0 < ext[0]; ++c0) {
                    if (--budget <= 0) {
                        return false;
                    }
                    const int x = (a[0] + c0) % dims[0];
                    const int y = (a[1] + c1) % dims[1];
                    const int z = (a[2] + c2) % dims[2];
                    const int id = z * dims[1] * dims[0] + y * dims[0] + x;
                    if (free.count(id) == 0 || (!mark && taken.count(id) > 0)) {
                        return false;
                    }
                    if (mark) {
                        taken.insert(id);
                    }
                }
            }
        }
        return true;
    };
    auto tdist = [&](const std::array<int, 3>& a, const std::array<int, 3>& b) {
        int s = 0;
        for (int d = 0; d < 3; ++d) {
            const int f = (b[d] - a[d] + dims[d]) % dims[d];
            s += std::min(f, dims[d] - f);
        }
        return s;
    };
    std::vector<std::array<int, 3>> placed(nb);
    for (const auto& a0 : anchors) {
        if (budget <= 0) {
            return {};
        }
        taken.clear();
        if (!brick_fits(a0, false) || !brick_fits(a0, true)) {
            continue;
        }
        placed[0] = a0;
        bool ok = true;
        for (int b = 1; b < nb && ok; ++b) {
            // Defrag-aware greedy: target each brick's IDEAL position -- its
            // logical predecessor's anchor shifted by the brick extent along
            // their shared cut axis (distance 0 reconstructs the contiguous
            // slab: seams torus-adjacent, no wiring needed). Plain
            // nearest-to-previous breaks distance ties by enumeration order
            // and can pick a perpendicular shift (unwirable seams) over the
            // seam-contiguous anchor.
            const std::array<int, 3> bi{b % cuts[0], (b / cuts[0]) % cuts[1],
                                        b / (cuts[0] * cuts[1])};
            int pred = b, pax = 0;
            for (int d = 0; d < 3; ++d) {
                if (bi[d] > 0) {
                    pax = d;
                    pred = b - (d == 0   ? 1
                                : d == 1 ? cuts[0]
                                         : cuts[0] * cuts[1]);
                    break;
                }
            }
            std::array<int, 3> ideal = placed[pred];
            ideal[pax] = (ideal[pax] + ext[pax]) % dims[pax];
            auto order = anchors;
            std::stable_sort(
                order.begin(), order.end(),
                [&](const std::array<int, 3>& u, const std::array<int, 3>& v) {
                    return tdist(ideal, u) < tdist(ideal, v);
                });
            ok = false;
            for (const auto& a : order) {
                if (budget <= 0) {
                    return {};
                }
                // Seam wirability needs perpendicular coords matching mod
                // block across bricks: only anchors congruent to brick 0's
                // residues qualify (face-aligned axes are all ≡0 anyway).
                bool cong = true;
                for (int d = 0; d < 3 && cong; ++d) {
                    cong = (a[d] - placed[0][d]) % block[d] == 0;
                }
                if (!cong) {
                    continue;
                }
                if (brick_fits(a, false) && brick_fits(a, true)) {
                    placed[b] = a;
                    ok = true;
                    break;
                }
            }
        }
        if (!ok) {
            continue;
        }
        // rank_map: DP-fastest linearization, shape axes straight onto torus
        // axes (matches place_bricks); brick linear index b2*(k0*k1)+b1*k0+b0.
        const int K = shape[0] * shape[1] * shape[2];
        std::vector<int> rank_map(K);
        for (int i = 0; i < K; ++i) {
            const std::array<int, 3> c{i % shape[0], (i / shape[0]) % shape[1],
                                       i / (shape[0] * shape[1])};
            const int bidx = (c[2] / ext[2]) * cuts[0] * cuts[1] +
                             (c[1] / ext[1]) * cuts[0] + (c[0] / ext[0]);
            std::array<int, 3> t{};
            for (int d = 0; d < 3; ++d) {
                t[d] = (placed[bidx][d] + c[d] % ext[d]) % dims[d];
            }
            rank_map[i] = t[2] * dims[1] * dims[0] + t[1] * dims[0] + t[0];
        }
        return rank_map;
    }
    return {};
}

// Generator-v2 search: all split specs (fewest bricks first) across all
// axis rotations, stopping at the first fully wired (L=0) hit. Returns the
// best candidate (lowest link load) and a spec tag like "2x1x1@p012".
std::optional<std::pair<RelaxedCandidate, std::string>> search_wired(
    const std::array<int, 3>& shape,
    const std::vector<std::pair<int, int>>& ring,
    const std::unordered_set<int>& free,
    const BlockModel& cm,
    const std::vector<int>& dims,
    const std::array<int, 3>& block,
    bool bidi) {
    static constexpr std::array<int, 3> kSpecs[] = {
        {2, 1, 1}, {1, 2, 1}, {1, 1, 2},  // 2 bricks
        {4, 1, 1}, {1, 4, 1}, {1, 1, 4},  // 4 bricks, single axis
        {2, 2, 1}, {2, 1, 2}, {1, 2, 2},  // 4 bricks, 2x2 grid
        {8, 1, 1}, {1, 8, 1}, {1, 1, 8}};
    // Axis rotations: the fold search tries permuted orientations, so the
    // probe must too or it understates feasibility under anisotropic free
    // space. The generator works in permuted space; the rank_map is then
    // reindexed back to the job's own linearization so ring/finalization see
    // the original shape. Equal-extent axis swaps give isomorphic physical
    // edge sets, so duplicate permuted shapes are skipped.
    static constexpr std::array<int, 3> kPerms[] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    std::optional<RelaxedCandidate> best;
    std::string best_spec = "-";
    int budget = 2000000;
    std::set<std::array<int, 6>> tried;
    for (const auto& perm : kPerms) {
        const std::array<int, 3> pshape{shape[perm[0]], shape[perm[1]],
                                        shape[perm[2]]};
        for (const auto& s : kSpecs) {
            if (best && best->link_load == 0) {
                break;
            }
            if (!tried
                     .insert(
                         {pshape[0], pshape[1], pshape[2], s[0], s[1], s[2]})
                     .second) {
                continue;
            }
            auto prm = place_bricks_v2(pshape, s, free, dims, block, budget);
            if (prm.empty()) {
                continue;
            }
            // Reindex: original rank i at shape coord c sits at permuted
            // coord c' with c'[j] = c[perm[j]].
            const int K = shape[0] * shape[1] * shape[2];
            std::vector<int> rank_map(K);
            for (int i = 0; i < K; ++i) {
                const std::array<int, 3> c{i % shape[0],
                                           (i / shape[0]) % shape[1],
                                           i / (shape[0] * shape[1])};
                const int ip =
                    c[perm[0]] +
                    pshape[0] * (c[perm[1]] + pshape[1] * c[perm[2]]);
                rank_map[i] = prm[ip];
            }
            auto rc =
                finalize_relaxed(std::move(rank_map), ring, cm, dims, bidi);
            if (!best || rc.link_load < best->link_load) {
                best = std::move(rc);
                best_spec = std::to_string(s[0]) + "x" + std::to_string(s[1]) +
                            "x" + std::to_string(s[2]) + "@p" +
                            std::to_string(perm[0]) + std::to_string(perm[1]) +
                            std::to_string(perm[2]);
            }
        }
        if (best && best->link_load == 0) {
            break;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    return std::make_pair(std::move(*best), std::move(best_spec));
}

// One probe record: run the v2 search plus the old aligned-slab generator
// for comparison, and append a CSV line.
void wired_probe(const JobInstance& job,
                 Tick now,
                 Tick waited,
                 const std::unordered_set<int>& free,
                 const std::vector<std::pair<int, int>>& ring,
                 const BlockModel& cm,
                 const std::vector<int>& dims,
                 const std::array<int, 3>& block,
                 bool bidi,
                 const char* path) {
    static std::ofstream out;
    static bool opened = false;
    if (!opened) {
        opened = true;
        out.open(path, std::ios::app);
        out << "tick,job_id,ranks,sx,sy,sz,waited_s,free,v2_found,v2_load,"
               "v2_edges,v2_wired,v2_spec,old_found,old_load,old_edges\n";
    }
    if (!out) {
        return;
    }
    auto found = search_wired(job.shape, ring, free, cm, dims, block, bidi);
    const RelaxedCandidate* best = found ? &found->first : nullptr;
    std::optional<RelaxedCandidate> old_best;
    int old_budget = 500000;
    for (int k : {2, 4}) {
        for (int axis = 0; axis < 3; ++axis) {
            if (job.shape[axis] < 2 * k || job.shape[axis] % k != 0) {
                continue;
            }
            auto rm =
                place_bricks(job.shape, axis, k, free, dims, block, old_budget);
            if (!rm.empty()) {
                auto rc = finalize_relaxed(std::move(rm), ring, cm, dims, bidi);
                if (!old_best || rc.link_load < old_best->link_load) {
                    old_best = std::move(rc);
                }
            }
        }
    }
    out << now << ',' << job.job_id << ',' << job.num_ranks << ','
        << job.shape[0] << ',' << job.shape[1] << ',' << job.shape[2] << ','
        << waited / 1000000000.0 << ',' << free.size() << ',' << (best ? 1 : 0)
        << ',' << (best ? best->link_load : -1) << ','
        << (best ? best->dor_edges : -1) << ','
        << (best ? static_cast<int>(best->plan.ocs_edges.size()) : -1) << ','
        << (found ? found->second : "-") << ',' << (old_best ? 1 : 0) << ','
        << (old_best ? old_best->link_load : -1) << ','
        << (old_best ? old_best->dor_edges : -1) << '\n';
}
}  // namespace

PlacementResult RFold::try_place(const JobInstance& job,
                                 const ClusterView& view) {
    PlacementResult r;
    const auto& dims = view.physical_dims();
    if (dims.size() != 3) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "rfold requires 3-D physical_dims (--npus-per-dim)";
        return r;
    }
    // The RDCN machinery requires an OCS to exist: at whole-torus block (the
    // folding-only arm) there are no inter-block links to re-wire, so strict
    // semantics (including the odd-ring DROP catalog) apply.
    const bool has_ocs =
        block_[0] < dims[0] || block_[1] < dims[1] || block_[2] < dims[2];
    BlockModel cm({dims[0], dims[1], dims[2]}, block_, has_ocs);
    if (!cm.valid()) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "--block-size must divide every torus dim per axis";
        return r;
    }
    if (job.num_ranks > view.total_npus()) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "num_ranks exceeds cluster size";
        return r;
    }

    // Fewer free NPUs than ranks: no fold variant, snake, or relaxed SFC
    // placement can possibly succeed (each needs num_ranks distinct free
    // NPUs, and relax-SFC DEFERs on free < ranks by its own logic), so skip
    // every search below. Only any_fits — which drives the DEFER/DROP oracle
    // decision and depends on the shape alone — still needs computing.
    const bool too_full =
        view.free_npus().size() < static_cast<size_t>(job.num_ranks);

    // The logical comm-ring edges depend only on the job shape (not the fold
    // variant), so compute them once and reuse for OCS scoring (in the
    // selector) and the final 1-hop routes.
    const auto ring = FootprintRouter::ring_edges(job.shape);

    // Per-decision ranker context: runtime queue state (when installed).
    // Cleared on every exit path — the ranker must never hold a pointer into
    // this stack frame.
    SchedContext ctx = runtime_ctx_ ? *runtime_ctx_ : SchedContext{};
    ranker_->set_context(&ctx);
    struct CtxClear {
        PlacementRanker* r;
        ~CtxClear() {
            r->set_context(nullptr);
        }
    } ctx_clear{ranker_.get()};

    bool any_fits = false;
    std::vector<Placement> candidates;
    if (too_full) {
        for (const auto& v : FoldEnumerator::enumerate(job.shape, multifold_)) {
            if (fits_cluster(v.footprint, dims)) {
                any_fits = true;
                break;
            }
        }
    } else {
        std::unordered_set<int> free(view.free_npus().begin(),
                                     view.free_npus().end());
        for (const auto& v : FoldEnumerator::enumerate(job.shape, multifold_)) {
            if (fits_cluster(v.footprint, dims)) {
                any_fits = true;
            }
            auto p =
                selector_->select(v, free, dims, cm, *scorer_, *ranker_, ring);
            if (p) {
                candidates.push_back(std::move(*p));
            }
        }
    }
    std::optional<Placement> best;
    int best_idx = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (best_idx < 0 ||
            ranker_->better(candidates[i], candidates[best_idx])) {
            best_idx = i;
        }
    }
    if (best_idx >= 0) {
        best = std::move(candidates[best_idx]);
    }

    if (best) {
        r.outcome = PlacementOutcome::PLACED;
        r.npus = best->rank_map;
        r.ordered_rings = true;  // Decision D4: folded job runs rings in order
        r.reconfig_plan.ocs_edges = best->ocs_edges;
        // Pin a 1-hop route for every ring edge that is torus-adjacent or
        // wired as an OCS link. Ring edges the OCS could not realize at this
        // position (best->dor_edges of them) stay unpinned and ride the
        // backend's standard DOR.
        r.reconfig_plan.routes = FootprintRouter::adjacent_routes(
            ring, best->rank_map, dims, best->ocs_edges);
        // Owned-links invariant: every installed route is a single physical hop
        // over a link the job owns -- either a baseline torus-adjacent link, or
        // an OCS link this plan wires. Catches a future regression (e.g. a
        // non-Ring collective, or a routing change) that would otherwise route
        // silently over a non-owned / multi-hop path. (assert is active in this
        // project's builds, like the SchedRuntime deadlock assert.)
        {
            std::set<std::pair<int, int>> ocs(r.reconfig_plan.ocs_edges.begin(),
                                              r.reconfig_plan.ocs_edges.end());
            for (const auto& row : r.reconfig_plan.routes) {
                assert(row.hops.size() == 2 && row.hops.front() == row.src &&
                       row.hops.back() == row.dst &&
                       "rfold route is not a single hop");
                const int a = std::min(row.src, row.dst);
                const int b = std::max(row.src, row.dst);
                assert((cm.is_torus_adjacent(row.src, row.dst) ||
                        ocs.count({a, b}) > 0) &&
                       "rfold route hop is neither torus-adjacent nor an owned "
                       "OCS link");
            }
            for (const auto& e : r.reconfig_plan.ocs_edges) {
                assert(cm.ocs_realizable_scatter(e.first, e.second) &&
                       "rfold OCS edge is not physically realizable");
            }
            assert(cm.directions_disjoint(r.reconfig_plan.ocs_edges) &&
                   "rfold OCS edges double-book a node direction");
        }
        return r;
    }
    // 1-D fallback hoisted from the old standalone folding policy. A job
    // with a single communicating ring can snake through arbitrary free
    // nodes (consecutive ranks and the wrap each 1 torus hop apart), so when
    // no fold variant fits the free NPUs right now, the snake is the
    // placement of last resort. Whether a failed snake means DEFER or DROP
    // is left to the idle-cluster oracle below (which mirrors this snake on
    // the idle torus): forcing any_fits here would skip the oracle and leave
    // a snake-budget failure DEFERring forever on an idle cluster, aborting
    // the sim at event-queue drain.
    const int non_unit =
        (job.shape[0] > 1) + (job.shape[1] > 1) + (job.shape[2] > 1);
    if (!too_full && non_unit <= 1) {
        auto cyc = FoldEnumerator::snake_1d(view.free_npus(), dims,
                                            job.num_ranks, kSnakeBudget);
        if (!cyc.empty()) {
            r.outcome = PlacementOutcome::PLACED;
            r.ordered_rings = true;
            // Every snake edge is torus-adjacent by construction: pin the
            // 1-hop routes; no OCS is wired.
            r.reconfig_plan.routes =
                FootprintRouter::adjacent_routes(ring, cyc, dims);
            // adjacent_routes silently skips non-1-hop edges, so check
            // completeness: a snake must pin every ring edge (both
            // directions) or it was not a 1-hop cycle.
            assert(r.reconfig_plan.routes.size() == ring.size() &&
                   "1-D snake left a ring edge unpinned (not a 1-hop cycle)");
            r.npus = std::move(cyc);
            return r;
        }
    }

    // Shape-constraint relaxation: no fold variant places this job right
    // now; once it has waited past the stall floor, starting it degraded
    // beats leaving it (and, under FIFO, everyone behind it) stalled. The
    // gate is deliberately minimal -- need (fold failed) AND accrued wait
    // >= relax-min-wait; full-length bidi validation showed every further
    // cap (link-load budget, stretch floor) filters grants toward small
    // jobs and blocks the head-of-line stallers that actually gate the
    // queue (design doc 10). Candidates, least relaxed first: brick-split
    // slabs (seams OCS-wired where realizable), then SFC scatter as the
    // placement of last resort; the lowest-link-load candidate wins and
    // its residual load is recorded as jobs.csv telemetry. All paths need
    // free >= ranks (too_full guards), so capacity stalls still DEFER.
    const Tick waited = view.current_tick() - job.arrival_time;
    if (!too_full && relax_on_ && waited >= relax_min_wait_ns_) {
        // The fold search's free set is scoped to its own branch (the
        // too_full fast path avoids building it); the gate runs only when
        // !too_full, so build one here.
        const std::unordered_set<int> free(view.free_npus().begin(),
                                           view.free_npus().end());
        std::optional<RelaxedCandidate> best_rc;
        auto consider = [&](std::vector<int> rank_map) {
            auto rc =
                finalize_relaxed(std::move(rank_map), ring, cm, dims, bidi_);
            if (!best_rc || rc.link_load < best_rc->link_load) {
                best_rc = std::move(rc);
            }
        };
        int brick_budget = kBrickBudget;
        for (int k : {2, 4}) {
            if (best_rc && best_rc->link_load == 0) {
                break;  // already fold-quality; nothing cheaper exists
            }
            for (int axis = 0; axis < 3; ++axis) {
                if (job.shape[axis] < 2 * k || job.shape[axis] % k != 0) {
                    continue;
                }
                auto rm = place_bricks(job.shape, axis, k, free, dims, block_,
                                       brick_budget);
                if (!rm.empty()) {
                    consider(std::move(rm));
                }
            }
        }
        if (!best_rc || best_rc->link_load > 0) {
            auto rr = relax_sfc_.try_place(job, view);
            if (rr.outcome == PlacementOutcome::PLACED) {
                consider(std::move(rr.npus));
            }
        }
        if (best_rc) {
            r.outcome = PlacementOutcome::PLACED;
            r.npus = std::move(best_rc->rank_map);
            r.ordered_rings = true;
            r.reconfig_plan = std::move(best_rc->plan);
            r.relaxed = true;
            r.relax_link_load = best_rc->link_load;
            LoggerFactory::get_logger("scheduling")
                ->info("job {} RELAXED: {} ranks at link load {} over {} "
                       "edges ({} seams wired; waited {}s)",
                       job.job_id, job.num_ranks, r.relax_link_load,
                       best_rc->dor_edges, r.reconfig_plan.ocs_edges.size(),
                       waited / 1000000000.0);
            return r;
        }
    }

    // A footprint that overflows the torus dims can still be placed by the
    // scatter path (blocks wired via OCS), so !any_fits alone cannot
    // distinguish "can never fit" from "cannot fit right now". Consult an
    // idle-cluster oracle before dropping. With permanently-failed NPUs the
    // converse gap opens too: a footprint that fits the torus dims may never
    // dodge the failed nodes, so under failures every defer consults the
    // oracle — which then answers for the degraded torus, keeping DROP
    // reserved for permanent impossibility.
    const auto& failed = view.failed_npus();
    if ((!any_fits || !failed.empty()) &&
        !placeable_on_idle(job, dims, cm, ring, failed)) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = failed.empty()
                       ? "no fold variant fits the cluster, even idle"
                       : "no fold variant fits the degraded cluster, even idle";
        return r;
    }
    // Wired-only feasibility probe: log-only diagnostic on long-wait shape
    // stalls (capacity stalls excluded via !too_full). Off unless the env
    // var is set, so normal runs never pay the free-set build.
    static const char* wired_probe_path = std::getenv("RFOLD_WIRED_PROBE");
    if (wired_probe_path != nullptr && !too_full && cm.ocs_enabled() &&
        waited >= relax_min_wait_ns_) {
        const std::unordered_set<int> probe_free(view.free_npus().begin(),
                                                 view.free_npus().end());
        wired_probe(job, view.current_tick(), waited, probe_free, ring, cm,
                    dims, block_, bidi_, wired_probe_path);
    }

    r.outcome = PlacementOutcome::DEFER;
    r.reason = "no fold variant fits free NPUs right now";
    return r;
}

bool RFold::placeable_on_idle(const JobInstance& job,
                              const std::vector<int>& dims,
                              BlockModel& cm,
                              const std::vector<std::pair<int, int>>& ring,
                              const std::unordered_set<int>& failed) {
    auto it = idle_ok_.find(job.shape);
    if (it != idle_ok_.end()) {
        return it->second;
    }
    // Idle = every usable NPU free; failed ids stay excluded for the whole
    // run, so a job unplaceable here is unplaceable forever.
    std::unordered_set<int> idle_free;
    const int n = dims[0] * dims[1] * dims[2];
    idle_free.reserve(n);
    std::vector<int> idle_vec;
    idle_vec.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (failed.find(i) == failed.end()) {
            idle_free.insert(i);
            idle_vec.push_back(i);
        }
    }
    bool ok = false;
    for (const auto& v : FoldEnumerator::enumerate(job.shape, multifold_)) {
        if (selector_->select(v, idle_free, dims, cm, *scorer_, *ranker_,
                              ring)) {
            ok = true;
            break;
        }
    }
    // Mirror try_place's placement of last resort: a 1-D job whose fold
    // variants all fail can still snake through arbitrary free nodes.
    const int non_unit =
        (job.shape[0] > 1) + (job.shape[1] > 1) + (job.shape[2] > 1);
    if (!ok && non_unit <= 1) {
        ok = !FoldEnumerator::snake_1d(idle_vec, dims, job.num_ranks,
                                       kSnakeBudget)
                  .empty();
    }
    idle_ok_.emplace(job.shape, ok);
    return ok;
}

}  // namespace Scheduling
}  // namespace AstraSim
