/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/BlockSelector.hh"

#include "astra-sim/scheduling/BlockModel.hh"
#include "astra-sim/scheduling/FoldEnumerator.hh"
#include "astra-sim/scheduling/FootprintRouter.hh"
#include "astra-sim/scheduling/FragmentationScorer.hh"
#include "astra-sim/scheduling/PlacementRanker.hh"

#include <gtest/gtest.h>
#include <unordered_set>
#include <vector>

using namespace AstraSim::Scheduling;

static std::unordered_set<int> full_free(int n) {
    std::unordered_set<int> s;
    for (int i = 0; i < n; ++i) {
        s.insert(i);
    }
    return s;
}

// Identity 2x2x2 fold variant.
static FoldVariant identity_2x2x2() {
    FoldVariant v;
    v.footprint = {2, 2, 2};
    v.ring_closes = true;
    for (int i = 0; i < 8; ++i) {
        v.embedding.push_back({i % 2, (i / 2) % 2, i / 4});
    }
    return v;
}

TEST(ContiguousFirst, PlacesIdentityWithinOneBlock) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto scorer = make_fragmentation_scorer("fewest-blocks", {4, 4, 4});
    ContiguousFirst sel;
    auto free = full_free(512);
    auto ring = FootprintRouter::ring_edges({2, 2, 2});
    CommFirst ranker;
    auto p = sel.select(identity_2x2x2(), free, {8, 8, 8}, cm, *scorer, ranker,
                        ring);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->rank_map.size(), 8u);
    EXPECT_TRUE(p->ocs_edges.empty());  // fits one 4-block -> no OCS links
}

TEST(ContiguousFirst, NoFitWhenNoFreeSpace) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto scorer = make_fragmentation_scorer("fewest-blocks", {4, 4, 4});
    ContiguousFirst sel;
    std::unordered_set<int> free;  // empty
    auto ring = FootprintRouter::ring_edges({2, 2, 2});
    CommFirst ranker;
    auto p = sel.select(identity_2x2x2(), free, {8, 8, 8}, cm, *scorer, ranker,
                        ring);
    EXPECT_FALSE(p.has_value());
}

TEST(BlockSelectorFactory, KnownAndUnknown) {
    EXPECT_NE(make_block_selector("contiguous"), nullptr);
    EXPECT_EQ(make_block_selector("nope"), nullptr);
    EXPECT_EQ(make_block_selector("fold-parity"), nullptr);  // deleted
}

// A length-4 ring fold variant (footprint 4x1x1, identity embedding).
static FoldVariant ring4_x() {
    FoldVariant v;
    v.footprint = {4, 1, 1};
    v.ring_closes = false;
    for (int i = 0; i < 4; ++i) {
        v.embedding.push_back({i, 0, 0});
    }
    return v;
}

TEST(ContiguousFirst, PlacesRealizableSelfWrap) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto scorer = make_fragmentation_scorer("fewest-blocks", {4, 4, 4});
    ContiguousFirst sel;
    auto free = full_free(512);
    auto ring = FootprintRouter::ring_edges({4, 1, 1});
    CommFirst ranker;
    auto p = sel.select(ring4_x(), free, {8, 8, 8}, cm, *scorer, ranker, ring);
    ASSERT_TRUE(p.has_value());
    // Block-aligned placement: the ring closure is a realizable block
    // self-wrap.
    ASSERT_EQ(p->ocs_edges.size(), 1u);
    EXPECT_TRUE(
        cm.ocs_realizable(p->ocs_edges[0].first, p->ocs_edges[0].second));
}

TEST(ContiguousFirst, PlacesNonRealizableClosureViaDor) {
    // Only free region is a mid-block x-line x=2..5. The length-4 ring there
    // needs a closure between interior nodes (x=5<->x=2), which no OCS can
    // realize. The candidate is still placed: the closure rides the backend's
    // standard DOR (dor_edges == 1) and no OCS link is wired. This keeps
    // contiguous placeability maximal: any cuboid fit is accepted.
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    auto scorer = make_fragmentation_scorer("fewest-blocks", {4, 4, 4});
    ContiguousFirst sel;
    std::unordered_set<int> free = {id(2, 0, 0), id(3, 0, 0), id(4, 0, 0),
                                    id(5, 0, 0)};
    auto ring = FootprintRouter::ring_edges({4, 1, 1});
    CommFirst ranker;
    auto p = sel.select(ring4_x(), free, {8, 8, 8}, cm, *scorer, ranker, ring);
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(p->ocs_edges.empty());
    EXPECT_EQ(p->dor_edges, 1);
}

TEST(ContiguousFirst, PrefersRealizableOverDorPlacement) {
    // Both a block-aligned line (x=0..3, closure = realizable self-wrap OCS)
    // and a mid-block line (x=2..5 at another y, closure unrealizable) are
    // free: the all-1-hop candidate must win regardless of fragmentation
    // score.
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    auto scorer = make_fragmentation_scorer("fewest-blocks", {4, 4, 4});
    ContiguousFirst sel;
    std::unordered_set<int> free = {id(0, 0, 0), id(1, 0, 0), id(2, 0, 0),
                                    id(3, 0, 0), id(2, 1, 0), id(3, 1, 0),
                                    id(4, 1, 0), id(5, 1, 0)};
    auto ring = FootprintRouter::ring_edges({4, 1, 1});
    CommFirst ranker;
    auto p = sel.select(ring4_x(), free, {8, 8, 8}, cm, *scorer, ranker, ring);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->dor_edges, 0);
    ASSERT_EQ(p->ocs_edges.size(), 1u);
    EXPECT_TRUE(
        cm.ocs_realizable(p->ocs_edges[0].first, p->ocs_edges[0].second));
}

TEST(MinReconfig, ReturnsContiguousDorPlacementWhenScatterImpossible) {
    // Same mid-block line as above; no whole free blocks exist, so scatter
    // cannot help -- MinReconfig must return the contiguous candidate with
    // its DOR-ridden closure rather than failing.
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    auto scorer = make_fragmentation_scorer("fewest-blocks", {4, 4, 4});
    MinReconfig sel(50000);
    std::unordered_set<int> free = {id(2, 0, 0), id(3, 0, 0), id(4, 0, 0),
                                    id(5, 0, 0)};
    auto ring = FootprintRouter::ring_edges({4, 1, 1});
    CommFirst ranker;
    auto p = sel.select(ring4_x(), free, {8, 8, 8}, cm, *scorer, ranker, ring);
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(p->ocs_edges.empty());
    EXPECT_EQ(p->dor_edges, 1);
}

namespace {
// Prefers scatter outright and disclaims the early-return guarantee —
// stands in for a ranker that lets scatter compete.
struct ScatterLover : public PlacementRanker {
    bool better(const Placement& a, const Placement& b) const override {
        if (a.scattered != b.scattered) {
            return a.scattered;
        }
        return a.cost < b.cost;
    }
    bool scatter_never_beats_closed() const override {
        return false;
    }
};
}  // namespace

// With scatter_never_beats_closed() == false, MinReconfig must let the
// scatter candidate compete even when a fully-closed contiguous fit exists.
TEST(MinReconfigGate, RankerCanPreferScatterOverClosedContiguous) {
    std::vector<int> dims = {8, 8, 8};
    BlockModel cm({8, 8, 8}, {2, 2, 2});
    std::unordered_set<int> free;
    for (int i = 0; i < 512; ++i) {
        free.insert(i);
    }
    auto vs = FoldEnumerator::enumerate({2, 2, 2});
    MinReconfig sel(50000);
    FewestBlocksTouched scorer({2, 2, 2});
    ScatterLover ranker;
    auto ring = FootprintRouter::ring_edges({2, 2, 2});
    auto p = sel.select(vs[0], free, dims, cm, scorer, ranker, ring);
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(p->scattered);
}

// Default rankers return the closed contiguous fit, never scatter. (Note:
// under CommFirst the outcome is gate-independent -- the tail comparison
// would also pick class-0 contiguous -- so this pins the behavior, while
// RankerCanPreferScatterOverClosedContiguous above pins the gate itself.)
TEST(MinReconfigGate, DefaultRankerReturnsClosedContiguousNotScatter) {
    std::vector<int> dims = {8, 8, 8};
    BlockModel cm({8, 8, 8}, {2, 2, 2});
    std::unordered_set<int> free;
    for (int i = 0; i < 512; ++i) {
        free.insert(i);
    }
    auto vs = FoldEnumerator::enumerate({2, 2, 2});
    MinReconfig sel(50000);
    FewestBlocksTouched scorer({2, 2, 2});
    CommFirst ranker;
    auto ring = FootprintRouter::ring_edges({2, 2, 2});
    auto p = sel.select(vs[0], free, dims, cm, scorer, ranker, ring);
    ASSERT_TRUE(p.has_value());
    EXPECT_FALSE(p->scattered);
}
