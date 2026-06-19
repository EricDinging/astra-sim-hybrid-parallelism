/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/BlockModel.hh"

#include <array>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

using AstraSim::Scheduling::BlockModel;

TEST(BlockModel, ValidDivisibility) {
    EXPECT_TRUE(BlockModel({8, 8, 8}, {4, 4, 4}).valid());
    EXPECT_FALSE(BlockModel({8, 8, 6}, {4, 4, 4}).valid());
    EXPECT_FALSE(BlockModel({8, 8, 8}, {0, 0, 0}).valid());
}

TEST(BlockModel, CoordAndBlock) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    // (x,y,z)=(5,2,1): id = 1*64 + 2*8 + 5 = 85
    EXPECT_EQ(cm.coord(85), (std::array<int, 3>{5, 2, 1}));
    EXPECT_EQ(cm.block_of(85), (std::array<int, 3>{1, 0, 0}));
}

TEST(BlockModel, SameBlockAndFace) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    EXPECT_TRUE(cm.same_block(id(0, 0, 0), id(3, 3, 3)));
    EXPECT_FALSE(cm.same_block(id(3, 0, 0), id(4, 0, 0)));
    EXPECT_TRUE(cm.is_face_node(id(3, 0, 0), 0, +1));
    EXPECT_TRUE(cm.is_face_node(id(4, 0, 0), 0, -1));
    EXPECT_FALSE(cm.is_face_node(id(2, 0, 0), 0, +1));
}

TEST(BlockModel, TorusAdjacency) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    EXPECT_TRUE(cm.is_torus_adjacent(id(3, 0, 0), id(4, 0, 0)));  // +1 in x
    EXPECT_TRUE(
        cm.is_torus_adjacent(id(0, 0, 0), id(7, 0, 0)));  // full-axis wrap
    EXPECT_FALSE(
        cm.is_torus_adjacent(id(0, 0, 0), id(3, 0, 0)));  // block self-wrap
    EXPECT_FALSE(cm.is_torus_adjacent(id(0, 0, 0), id(1, 1, 0)));  // diagonal
    EXPECT_FALSE(cm.is_torus_adjacent(id(0, 0, 0), id(0, 0, 0)));  // same node
}

TEST(BlockModel, OcsRealizable) {
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    // Block self-wrap on x: -X face (x=0) <-> +X face (x=3), same y,z -> OK.
    EXPECT_TRUE(cm.ocs_realizable(id(0, 0, 0), id(3, 0, 0)));
    // Inter-block faces: +X of block0 (x=3) <-> -X of block1 (x=4) -> OK.
    EXPECT_TRUE(cm.ocs_realizable(id(3, 1, 2), id(4, 1, 2)));
    // Mid-block interior nodes (x=2,x=5; within-block 2 and 1, not faces) ->
    // no.
    EXPECT_FALSE(cm.ocs_realizable(id(2, 0, 0), id(5, 0, 0)));
    // Differs in two dims -> not a single OCS link.
    EXPECT_FALSE(cm.ocs_realizable(id(0, 0, 0), id(3, 3, 0)));
}

TEST(BlockModel, NonCubicBlock) {
    // 4x4x2 blocks on an 8^3 torus: a 2x2x4 grid of blocks; z-faces live at
    // within-block z == 1 (+z) and 0 (-z). Exercises per-axis block geometry.
    BlockModel cm({8, 8, 8}, {4, 4, 2});
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    EXPECT_TRUE(cm.valid());  // 8%4, 8%4, 8%2 == 0
    EXPECT_FALSE(BlockModel({8, 8, 8}, {4, 4, 3}).valid());  // 8 % 3 != 0
    EXPECT_EQ(cm.block_of(id(5, 2, 3)), (std::array<int, 3>{1, 0, 1}));
    EXPECT_TRUE(cm.is_face_node(id(0, 0, 1), 2, +1));  // within-block z=1 = +z
    EXPECT_TRUE(cm.is_face_node(id(0, 0, 2), 2, -1));  // within-block z=0 = -z
    EXPECT_FALSE(cm.is_face_node(id(0, 0, 1), 2, -1));
    // scatter z-OCS across non-adjacent z-blocks: +z face (z=1) <-> -z face
    // (z=6) with matching within-block x,y -> realizable.
    EXPECT_TRUE(cm.ocs_realizable_scatter(id(0, 0, 1), id(0, 0, 6)));
    // both at within-block z=1 (same face, not opposite) -> not realizable.
    EXPECT_FALSE(cm.ocs_realizable_scatter(id(0, 0, 1), id(0, 0, 7)));
}

TEST(BlockModel, DirectionsDisjointUnitBlock) {
    // 1x1x1 blocks: every node exposes both faces of every axis, so all 6 of
    // its ports are OCS-attached and an edge may ride any axis group. A node
    // can therefore terminate up to 6 OCS edges (e.g. a rank in 3 rings);
    // only a 7th double-books a port.
    BlockModel cm({8, 8, 8}, {1, 1, 1});
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    // chain 0-2-4-6 along x: middle nodes carry two same-axis edges -> OK.
    EXPECT_TRUE(cm.directions_disjoint({{id(0, 0, 0), id(2, 0, 0)},
                                        {id(2, 0, 0), id(4, 0, 0)},
                                        {id(4, 0, 0), id(6, 0, 0)}}));
    // ring closure 0-2-4-0 (a directed cycle): still one port pair per node.
    EXPECT_TRUE(cm.directions_disjoint({{id(0, 0, 0), id(2, 0, 0)},
                                        {id(2, 0, 0), id(4, 0, 0)},
                                        {id(4, 0, 0), id(0, 0, 0)}}));
    // 6 edges at node (2,0,0) -- a rank in three rings, every neighbor
    // non-adjacent: fills all 6 ports, still realizable.
    std::vector<std::pair<int, int>> six;
    for (int k = 1; k <= 6; ++k) {
        six.push_back({id(2, 0, 0), id(2, 2, k)});
    }
    EXPECT_TRUE(cm.directions_disjoint(six));
    // a 7th edge at the same node has no port left.
    six.push_back({id(2, 0, 0), id(5, 5, 5)});
    EXPECT_FALSE(cm.directions_disjoint(six));
}

TEST(BlockModel, DirectionsDisjointWideBlockUnchanged) {
    // block >= 2 on the axis: face membership pins the port, so two edges
    // sharing a face node on the same axis still collide.
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    // node (3,0,0) is a +x face node; two x-OCS edges there double-book +x.
    EXPECT_FALSE(cm.directions_disjoint(
        {{id(3, 0, 0), id(4, 0, 0)}, {id(3, 0, 0), id(0, 0, 0)}}));
    // node (4,0,0) is a -x face node; two x-OCS edges double-book -x.
    EXPECT_FALSE(cm.directions_disjoint(
        {{id(3, 0, 0), id(4, 0, 0)}, {id(4, 0, 0), id(7, 0, 0)}}));
    // disjoint ports stay fine.
    EXPECT_TRUE(cm.directions_disjoint(
        {{id(3, 0, 0), id(4, 0, 0)}, {id(0, 1, 0), id(3, 1, 0)}}));
}
