/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/BlockModel.hh"

#include <array>
#include <gtest/gtest.h>
#include <unordered_set>
#include <utility>
#include <vector>

using AstraSim::Scheduling::BlockModel;

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
}  // namespace

TEST(BlockModelScatter, IdOfRoundTrip) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    EXPECT_EQ(cm.id_of({5, 2, 1}), 85);
    EXPECT_EQ(cm.id_of(cm.coord(300)), 300);
}

TEST(BlockModelScatter, RealizableAcrossArbitraryBlocks) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    EXPECT_TRUE(cm.ocs_realizable_scatter(id8(3, 1, 2), id8(4, 5, 6)));
    EXPECT_FALSE(cm.ocs_realizable_scatter(id8(3, 1, 2), id8(4, 5, 7)));
    EXPECT_FALSE(cm.ocs_realizable_scatter(id8(2, 0, 0), id8(5, 0, 0)));
    // self-edge is never realizable
    EXPECT_FALSE(cm.ocs_realizable_scatter(id8(3, 0, 0), id8(3, 0, 0)));
    // superset: anything ocs_realizable accepts, scatter accepts too
    EXPECT_TRUE(cm.ocs_realizable(id8(3, 0, 0), id8(4, 0, 0)));
    EXPECT_TRUE(cm.ocs_realizable_scatter(id8(3, 0, 0), id8(4, 0, 0)));
}

TEST(BlockModelScatter, FreeNeighborBlocks) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    std::unordered_set<int> free;
    add_block(free, 1, 0, 0);
    EXPECT_EQ(cm.free_neighbor_blocks({0, 0, 0}, free), 1);
    EXPECT_EQ(cm.free_neighbor_blocks({1, 1, 1}, free), 0);
}

TEST(BlockModelScatter, DirectionsDisjoint) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    int u = id8(3, 0, 0);
    std::vector<std::pair<int, int>> conflict = {{u, id8(4, 0, 0)},
                                                 {u, id8(4, 4, 0)}};
    EXPECT_FALSE(cm.directions_disjoint(conflict));
    std::vector<std::pair<int, int>> ok = {{id8(3, 0, 0), id8(4, 0, 0)},
                                           {id8(3, 4, 0), id8(4, 4, 0)}};
    EXPECT_TRUE(cm.directions_disjoint(ok));
}
