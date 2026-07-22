/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/ScatterAssigner.hh"

#include "astra-sim/scheduling/BlockModel.hh"
#include "astra-sim/scheduling/FoldEnumerator.hh"
#include "astra-sim/scheduling/FootprintRouter.hh"

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
}  // namespace

TEST(ScatterAssigner, PlacesAcrossNonAdjacentBlocks) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto v = ident({8, 4, 4});  // 2 full blocks along x
    std::unordered_set<int> free;
    add_block(free, 0, 0, 0);
    add_block(free, 1, 1, 1);  // diagonal: no contiguous fit
    auto ring = FootprintRouter::ring_edges({8, 4, 4});
    auto rm = scatter_assign(v, free, {8, 8, 8}, cm, ring, 100000);
    ASSERT_TRUE(rm.has_value());
    EXPECT_EQ(rm->size(), 128u);
    std::unordered_set<int> uniq(rm->begin(), rm->end());
    EXPECT_EQ(uniq.size(), 128u);  // distinct
    for (int id : *rm) {
        EXPECT_TRUE(free.count(id) > 0);  // all free
    }
    auto oe = FootprintRouter::ocs_edges(ring, *rm, cm);
    EXPECT_FALSE(oe.empty());
    for (const auto& e : oe) {
        EXPECT_TRUE(cm.ocs_realizable_scatter(e.first, e.second));
    }
}

TEST(ScatterAssigner, NulloptWhenNotEnoughBlocks) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto v = ident({8, 4, 4});  // needs 2 blocks
    std::unordered_set<int> free;
    add_block(free, 0, 0, 0);  // only 1 block free
    auto ring = FootprintRouter::ring_edges({8, 4, 4});
    auto rm = scatter_assign(v, free, {8, 8, 8}, cm, ring, 100000);
    EXPECT_FALSE(rm.has_value());
}

TEST(ScatterAssigner, NulloptWhenBudgetZero) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto v = ident({8, 4, 4});
    std::unordered_set<int> free;
    add_block(free, 0, 0, 0);
    add_block(free, 1, 1, 1);
    auto ring = FootprintRouter::ring_edges({8, 4, 4});
    auto rm = scatter_assign(v, free, {8, 8, 8}, cm, ring, 0);
    EXPECT_FALSE(rm.has_value());
}

TEST(ScatterAssigner, SlabLeavesRestOfBlockFree) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto v = ident({4, 4, 1});  // a single slab (one z-layer)
    std::unordered_set<int> free;
    add_block(free, 0, 0, 0);
    auto ring = FootprintRouter::ring_edges({4, 4, 1});
    auto rm = scatter_assign(v, free, {8, 8, 8}, cm, ring, 100000);
    ASSERT_TRUE(rm.has_value());
    EXPECT_EQ(rm->size(), 16u);  // only the slab's NPUs are consumed
    int z0 = cm.coord((*rm)[0])[2];
    for (int id : *rm) {
        EXPECT_EQ(cm.coord(id)[2], z0);
    }
}
