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

#include <array>
#include <gtest/gtest.h>
#include <unordered_set>

using namespace AstraSim::Scheduling;

namespace {
int id8(int x, int y, int z) {
    return z * 64 + y * 8 + x;
}
void add_block(std::unordered_set<int>& free, int cx, int cy, int cz) {
    for (int z = cz * 4; z < cz * 4 + 4; ++z) {
        for (int y = cy * 4; y < cy * 4 + 4; ++y) {
            for (int x = cx * 4; x < cx * 4 + 4; ++x) {
                free.insert(id8(x, y, z));
            }
        }
    }
}
FoldVariant ident(std::array<int, 3> fp) {
    FoldVariant v;
    v.footprint = fp;
    v.ring_closes = true;
    int n = fp[0] * fp[1] * fp[2];
    for (int i = 0; i < n; ++i) {
        v.embedding.push_back(
            {i % fp[0], (i / fp[0]) % fp[1], i / (fp[0] * fp[1])});
    }
    return v;
}
std::unordered_set<int> full_free(int n) {
    std::unordered_set<int> s;
    for (int i = 0; i < n; ++i) {
        s.insert(i);
    }
    return s;
}
}  // namespace

TEST(Phase2Factory, KnownNames) {
    EXPECT_NE(make_block_selector("min-reconfig"), nullptr);
    EXPECT_EQ(make_block_selector("nope"), nullptr);
}

TEST(MinReconfig, UsesContiguousWhenAvailable) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto scorer = make_fragmentation_scorer("fewest-blocks", {4, 4, 4});
    MinReconfig sel(50000);
    auto free = full_free(512);
    auto ring = FootprintRouter::ring_edges({2, 2, 2});
    CommFirst ranker;
    auto p = sel.select(ident({2, 2, 2}), free, {8, 8, 8}, cm, *scorer, ranker,
                        ring);
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(p->ocs_edges.empty());  // contiguous within one block -> 0 OCS
}

TEST(MinReconfig, ScattersWhenNoContiguousFit) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto scorer = make_fragmentation_scorer("fewest-blocks", {4, 4, 4});
    MinReconfig sel(50000);
    std::unordered_set<int> free;
    add_block(free, 0, 0, 0);
    add_block(free, 1, 1, 1);  // diagonal: no contiguous (or rotated) fit
    auto ring = FootprintRouter::ring_edges({8, 4, 4});
    CommFirst ranker;
    auto p = sel.select(ident({8, 4, 4}), free, {8, 8, 8}, cm, *scorer, ranker,
                        ring);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->rank_map.size(), 128u);
    EXPECT_FALSE(p->ocs_edges.empty());
    for (const auto& e : p->ocs_edges) {
        EXPECT_TRUE(cm.ocs_realizable_scatter(e.first, e.second));
    }
}

TEST(MinReconfig, NulloptWhenScatterImpossible) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto scorer = make_fragmentation_scorer("fewest-blocks", {4, 4, 4});
    MinReconfig sel(50000);
    std::unordered_set<int> free;
    add_block(free, 0, 0, 0);  // only 1 block free, job needs 2
    auto ring = FootprintRouter::ring_edges({8, 4, 4});
    CommFirst ranker;
    auto p = sel.select(ident({8, 4, 4}), free, {8, 8, 8}, cm, *scorer, ranker,
                        ring);
    EXPECT_FALSE(p.has_value());
}
