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
#include <map>
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
// by PlacementRanker.
int count_blocks(const std::vector<int>& npus, const BlockModel& cm) {
    std::set<std::array<int, 3>> blocks;
    for (int n : npus) {
        blocks.insert(cm.block_of(n));
    }
    return static_cast<int>(blocks.size());
}

// Blocks the placement newly fragments: every chip free before, partially
// occupied after. P chips placed into a block with F free of V total: idle
// before iff F == V; fully consumed after iff P == F.
int frag_delta_of(const std::vector<int>& rm,
                  const BlockModel& cm,
                  const std::unordered_set<int>& free) {
    std::map<std::array<int, 3>, int> placed_in;
    for (int n : rm) {
        ++placed_in[cm.block_of(n)];
    }
    const auto blk = cm.block_dims();
    const int vol = blk[0] * blk[1] * blk[2];
    int frag = 0;
    for (const auto& [b, p] : placed_in) {
        int f = 0;
        for (int z = b[2] * blk[2]; z < (b[2] + 1) * blk[2]; ++z) {
            for (int y = b[1] * blk[1]; y < (b[1] + 1) * blk[1]; ++y) {
                for (int x = b[0] * blk[0]; x < (b[0] + 1) * blk[0]; ++x) {
                    f += free.count(cm.id_of({x, y, z})) > 0 ? 1 : 0;
                }
            }
        }
        if (f == vol && p < f) {
            ++frag;
        }
    }
    return frag;
}

Placement build_placement(const std::vector<int>& rm,
                          const FoldVariant& v,
                          const std::vector<int>& dims,
                          const BlockModel& cm,
                          const FragmentationScorer& scorer,
                          const std::vector<std::pair<int, int>>& ring,
                          int frag_delta,
                          int blocks_touched) {
    auto oe = FootprintRouter::ocs_edges(ring, rm, cm);
    // RDCN mode (DOR-tolerant glue): wire only the realizable, port-disjoint
    // subset; the rest ride the shared fabric and are charged as dor_edges.
    // No-OCS (folding-only) mode: scatter_assign's gate guarantees every edge
    // is realizable and wirable_subset is the identity, so wired == oe and
    // dor == 0.
    std::vector<std::pair<int, int>> wired;
    for (const auto& e : oe) {
        if (cm.ocs_realizable_scatter(e.first, e.second)) {
            wired.push_back(e);
        }
    }
    std::vector<std::pair<int, int>> phys_edges;
    phys_edges.reserve(ring.size());
    for (const auto& e : ring) {
        phys_edges.push_back({rm[e.first], rm[e.second]});
    }
    wired = cm.wirable_subset(phys_edges, wired);
    const int dor = static_cast<int>(oe.size() - wired.size());
    ScoredPlacement sp{&rm, &dims, v.footprint};
    double cost = scorer.cost(sp);
    Placement p{rm, v.footprint, std::move(wired), cost};
    p.blocks_touched = blocks_touched;
    p.dor_edges = dor;
    p.frag_delta = frag_delta;
    p.identity = v.identity;
    p.ring_closes = v.ring_closes;
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
            // Cheap ranking prefix first: when the incumbent already wins on
            // (frag_delta, blocks_touched) alone, skip the expensive OCS
            // realizability / wirable-subset / scorer work entirely (exact
            // for lexicographic rankers; others always take the full path).
            const int frag = frag_delta_of(cand, cm, free);
            const int blocks = count_blocks(cand, cm);
            if (result && ranker.prefix_beats(*result, frag, blocks)) {
                return;
            }
            auto oe = FootprintRouter::ocs_edges(ring_edges, cand, cm);
            // Wire OCS only where it is realizable (opposite block faces at
            // the same position). Ring edges the OCS cannot serve at this
            // position (open-fold seams, non-block-aligned closures) are NOT
            // grounds for rejection: they simply ride the backend's standard
            // DOR, and the candidate is deprioritized via dor_edges. This
            // keeps contiguous placeability maximal: any cuboid fit is
            // accepted.
            std::vector<std::pair<int, int>> realizable;
            for (const auto& e : oe) {
                if (cm.ocs_realizable(e.first, e.second)) {
                    realizable.push_back(e);
                }
            }
            // RDCN R5 (OCS mode): a wire whose face port is occupied by a
            // default-seam ride of this same placement cannot be installed —
            // it rides DOR instead. Identity in no-OCS mode.
            std::vector<std::pair<int, int>> phys_edges;
            phys_edges.reserve(ring_edges.size());
            for (const auto& e : ring_edges) {
                phys_edges.push_back({cand[e.first], cand[e.second]});
            }
            realizable = cm.wirable_subset(phys_edges, realizable);
            int dor = static_cast<int>(oe.size() - realizable.size());
            ScoredPlacement sp{&cand, &dims, pf};
            double c = scorer.cost(sp);
            Placement p{cand, pf, std::move(realizable), c};
            p.blocks_touched = blocks;
            p.dor_edges = dor;
            p.frag_delta = frag;
            p.identity = v.identity;
            p.ring_closes = v.ring_closes;
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
    // Rankers that let scatter compete opt out by returning false from
    // scatter_never_beats_closed(), which skips the early return so the
    // scatter candidate can enter the comparison.
    ContiguousFirst cf;
    auto c = cf.select(v, free, dims, cm, scorer, ranker, ring_edges);
    if (c && c->dor_edges == 0 && ranker.scatter_never_beats_closed()) {
        return c;
    }
    // Scatter fallback, over the distinct axis rotations of the variant (the
    // contiguous scan rotates internally; scatter must too, or rotated glue
    // placements are silently unreachable). The assigner tries blocks in
    // ascending block-coord order -> first-fit block order (fewest
    // reconfigured links).
    std::optional<Placement> s;
    std::array<int, 3> ax{0, 1, 2};
    std::set<std::array<int, 3>> tried_fp;
    do {  // identity orientation first, then the other axis rotations
        FoldVariant pv = permute_variant(v, ax);
        if (!tried_fp.insert(pv.footprint).second) {
            continue;  // symmetric footprint: identical tiling already tried
        }
        // RDCN mode: tile nesting (per-tile offsets, block sharing,
        // fragmented-block preference). No-OCS (folding-only) mode: the
        // original origin-locked, one-tile-per-block assigner.
        auto rm =
            cm.ocs_enabled()
                ? scatter_assign_nested(pv, free, dims, cm, ring_edges, budget_)
                : scatter_assign(pv, free, dims, cm, ring_edges, budget_);
        if (!rm) {
            continue;
        }
        // Same cheap-prefix shortcut as the contiguous scan: skip the OCS /
        // wirable-subset / scorer work for a rotation that already lost.
        const int frag = frag_delta_of(*rm, cm, free);
        const int blocks = count_blocks(*rm, cm);
        if (s && ranker.prefix_beats(*s, frag, blocks)) {
            continue;
        }
        auto p = build_placement(*rm, pv, dims, cm, scorer, ring_edges, frag,
                                 blocks);
        if (!s || ranker.better(p, *s)) {
            s = std::move(p);
        }
    } while (std::next_permutation(ax.begin(), ax.end()));
    if (!s) {
        return c;  // possibly nullopt, possibly contiguous-with-DOR-edges
    }
    if (c && ranker.better(*c, *s)) {
        return c;
    }
    return s;
}

std::unique_ptr<BlockSelector> make_block_selector(const std::string& name,
                                                   int search_budget) {
    if (name == "contiguous") {
        return std::make_unique<ContiguousFirst>();
    }
    if (name == "min-reconfig") {
        return std::make_unique<MinReconfig>(search_budget);
    }
    return nullptr;
}

}  // namespace Scheduling
}  // namespace AstraSim
