/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/BlockSelector.hh"

#include "astra-sim/scheduling/ContiguousScan.hh"
#include "astra-sim/scheduling/FootprintRouter.hh"
#include "astra-sim/scheduling/PlacementRanker.hh"
#include "astra-sim/scheduling/ScatterAssigner.hh"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace AstraSim {
namespace Scheduling {

namespace {

// Axis-permuted copy of a fold variant (footprint and every embedding
// coordinate; ring edges are rank pairs and stay valid). The contiguous
// scan rotates internally, but scatter historically tiled only the
// variant's given orientation, silently dropping rotated glue placements
// (e.g. an as-is 2x6x8 whose free space fits 8x6x2).
FoldVariant permute_variant(const FoldVariant& v,
                            const std::array<int, 3>& ax) {
    FoldVariant p;
    p.footprint = {v.footprint[ax[0]], v.footprint[ax[1]], v.footprint[ax[2]]};
    p.embedding.reserve(v.embedding.size());
    for (const auto& e : v.embedding) {
        p.embedding.push_back({e[ax[0]], e[ax[1]], e[ax[2]]});
    }
    p.ring_closes = v.ring_closes;
    p.identity = v.identity;
    return p;
}

// Distinct A×B×C blocks a placement's NPUs intersect -- the packing key used
// by PlacementRanker (kept separate from the scorer's bundled cost, which also
// penalizes OCS links below its leading block-count term).
int count_blocks(const std::vector<int>& npus, const BlockModel& cm) {
    std::set<std::array<int, 3>> blocks;
    for (int n : npus) {
        blocks.insert(cm.block_of(n));
    }
    return static_cast<int>(blocks.size());
}

// Sum of free_neighbor_blocks over the distinct blocks touched by `npus`,
// evaluated against the PRE-placement `free` set. Lower means the placement
// consumes blocks that are already isolated (wall-hugging tiebreak).
int residual_frag_of(const std::vector<int>& npus,
                     const BlockModel& cm,
                     const std::unordered_set<int>& free) {
    std::set<std::array<int, 3>> blocks;
    for (int n : npus) {
        blocks.insert(cm.block_of(n));
    }
    int r = 0;
    for (const auto& b : blocks) {
        r += cm.free_neighbor_blocks(b, free);
    }
    return r;
}

Placement build_placement(const std::vector<int>& rm,
                          const FoldVariant& v,
                          const std::vector<int>& dims,
                          const BlockModel& cm,
                          const FragmentationScorer& scorer,
                          const std::vector<std::pair<int, int>>& ring,
                          const std::unordered_set<int>& free) {
    auto oe = FootprintRouter::ocs_edges(ring, rm, cm);
    ScoredPlacement sp{&rm, &dims, v.footprint};
    sp.ocs_links = static_cast<int>(oe.size());
    double cost = scorer.cost(sp);
    Placement p{rm, v.footprint, std::move(oe), cost};
    p.blocks_touched = count_blocks(rm, cm);
    p.identity = v.identity;
    p.ring_closes = v.ring_closes;
    p.residual_frag = residual_frag_of(rm, cm, free);
    p.scattered = true;
    return p;
}

}  // namespace

std::optional<Placement> ContiguousFirst::select(
    const FoldVariant& v,
    const std::unordered_set<int>& free,
    const std::vector<int>& dims,
    const BlockModel& cm,
    const FragmentationScorer& scorer,
    const PlacementRanker& ranker,
    const std::vector<std::pair<int, int>>& ring_edges) const {
    const int K = static_cast<int>(v.embedding.size());

    bool any_fits = false;  // unused here; RFold computes DEFER/DROP itself
    std::optional<Placement> result;
    scan_contiguous_fits(
        v, dims, free, K, any_fits,
        [&](const std::vector<int>& cand, const std::array<int, 3>& pf) {
            auto oe = FootprintRouter::ocs_edges(ring_edges, cand, cm);
            // Wire OCS only where it is realizable (opposite block faces at
            // the same position). Ring edges the OCS cannot serve at this
            // position (open-fold seams, non-block-aligned closures) are NOT
            // grounds for rejection: they simply ride the backend's standard
            // DOR, and the candidate is deprioritized via dor_edges. This
            // keeps contiguous placeability maximal: any cuboid fit is
            // accepted.
            std::vector<std::pair<int, int>> realizable;
            int dor = 0;
            for (const auto& e : oe) {
                if (cm.ocs_realizable(e.first, e.second)) {
                    realizable.push_back(e);
                } else {
                    ++dor;
                }
            }
            ScoredPlacement sp{&cand, &dims, pf};
            sp.ocs_links = static_cast<int>(realizable.size());
            double c = scorer.cost(sp);
            Placement p{cand, pf, std::move(realizable), c};
            p.blocks_touched = count_blocks(cand, cm);
            p.dor_edges = dor;
            p.identity = v.identity;
            p.ring_closes = v.ring_closes;
            p.residual_frag = residual_frag_of(cand, cm, free);
            if (!result || ranker.better(p, *result)) {
                result = std::move(p);
            }
        });
    return result;
}

std::optional<Placement> MinReconfig::select(
    const FoldVariant& v,
    const std::unordered_set<int>& free,
    const std::vector<int>& dims,
    const BlockModel& cm,
    const FragmentationScorer& scorer,
    const PlacementRanker& ranker,
    const std::vector<std::pair<int, int>>& ring_edges) const {
    // Contiguous first: never scatter when a cuboid fit exists. A contiguous
    // candidate may carry DOR-ridden edges (dor_edges > 0) when the OCS cannot
    // realize a seam/closure at that position; the scatter is then compared via
    // the ranker.
    // Fast path: if the ranker guarantees scatter never beats a fully-closed
    // contiguous placement (dor_edges == 0), return immediately. This is a
    // ranker contract:
    //   - comm-first: scatter is the last class, so the guarantee holds.
    //   - packing-first: the early return IS the legacy behavior.
    // Rankers that let scatter compete (cost-model with the scatter guard off)
    // opt out by returning false from scatter_never_beats_closed(), which
    // skips the early return so the scatter candidate can enter the comparison.
    ContiguousFirst cf;
    auto c = cf.select(v, free, dims, cm, scorer, ranker, ring_edges);
    if (c && c->dor_edges == 0 && ranker.scatter_never_beats_closed()) {
        return c;
    }
    // Scatter fallback, over the distinct axis rotations of the variant (the
    // contiguous scan rotates internally; scatter must too, or rotated glue
    // placements are silently unreachable). A constant block_rank makes the
    // assigner fall back to its block-coord tiebreak -> first-fit block order
    // (fewest reconfigured links).
    std::optional<Placement> s;
    std::array<int, 3> ax{0, 1, 2};
    std::set<std::array<int, 3>> tried_fp;
    do {  // identity orientation first; rotations only when rotate_scatter_
        FoldVariant pv = permute_variant(v, ax);
        if (!tried_fp.insert(pv.footprint).second) {
            continue;  // symmetric footprint: identical tiling already tried
        }
        auto rm = scatter_assign(
            pv, free, dims, cm, ring_edges,
            [](const std::array<int, 3>&) { return 0.0; }, budget_);
        if (!rm) {
            continue;
        }
        auto p = build_placement(*rm, pv, dims, cm, scorer, ring_edges, free);
        if (!s || ranker.better(p, *s)) {
            s = std::move(p);
        }
    } while (rotate_scatter_ && std::next_permutation(ax.begin(), ax.end()));
    if (!s) {
        return c;  // possibly nullopt, possibly contiguous-with-DOR-edges
    }
    if (c && ranker.better(*c, *s)) {
        return c;
    }
    return s;
}

std::optional<Placement> MaxDefrag::select(
    const FoldVariant& v,
    const std::unordered_set<int>& free,
    const std::vector<int>& dims,
    const BlockModel& cm,
    const FragmentationScorer& scorer,
    const PlacementRanker& /*ranker*/,
    const std::vector<std::pair<int, int>>& ring_edges) const {
    auto rm = scatter_assign(
        v, free, dims, cm, ring_edges,
        [&](const std::array<int, 3>& c) {
            return static_cast<double>(cm.free_neighbor_blocks(c, free));
        },
        budget_);
    if (!rm) {
        return std::nullopt;
    }
    return build_placement(*rm, v, dims, cm, scorer, ring_edges, free);
}

std::unique_ptr<BlockSelector> make_block_selector(const std::string& name,
                                                   int search_budget,
                                                   bool rotate_scatter) {
    if (name == "contiguous") {
        return std::make_unique<ContiguousFirst>();
    }
    if (name == "min-reconfig") {
        return std::make_unique<MinReconfig>(search_budget, rotate_scatter);
    }
    if (name == "max-defrag") {
        return std::make_unique<MaxDefrag>(search_budget);
    }
    return nullptr;
}

}  // namespace Scheduling
}  // namespace AstraSim
