/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/FootprintRouter.hh"

#include "astra-sim/scheduling/BlockModel.hh"

#include <array>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

using namespace AstraSim::Scheduling;

TEST(FootprintRouter, RingEdgesForShape) {
    // Shape 2x2x1 (DP=2, TP=2): dp ring {0-1, 2-3}, tp ring {0-2, 1-3} -> 4
    // undirected -> 8 directed. The size-1 dim contributes no ring.
    auto edges = FootprintRouter::ring_edges({2, 2, 1});
    EXPECT_EQ(edges.size(), 8u);
}

TEST(FootprintRouter, OcsEdgesAreNonAdjacentEdges) {
    // An OCS edge is a ring edge whose endpoints are NOT one torus hop apart
    // (no baseline link) -- it must be wired as a real 1-hop OCS link.
    BlockModel cm({8, 8, 8}, {4, 4, 4});
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };

    // (a) A length-4 ring (shape 4x1x1) placed at x=0..3 in one block. Steps
    // 0-1,1-2,2-3 are torus-adjacent (baseline links); the closure 3->0 spans
    // the full block axis (3 apart, NOT adjacent) -> exactly 1 OCS edge (a
    // block self-wrap).
    std::vector<int> rm = {id(0, 0, 0), id(1, 0, 0), id(2, 0, 0), id(3, 0, 0)};
    auto e1 = FootprintRouter::ring_edges({4, 1, 1});
    auto o1 = FootprintRouter::ocs_edges(e1, rm, cm);
    ASSERT_EQ(o1.size(), 1u);
    EXPECT_EQ(o1[0], (std::pair<int, int>{id(0, 0, 0), id(3, 0, 0)}));
    EXPECT_FALSE(cm.is_torus_adjacent(o1[0].first, o1[0].second));

    // (b) A 2x2x1 job straddling the x=3|4 block boundary: every ring edge is
    // torus-adjacent (incl. the cross-block x=3<->x=4 step, which the baseline
    // torus already links) -> NO OCS edges needed.
    std::vector<int> rm2 = {id(3, 0, 0), id(4, 0, 0), id(3, 1, 0), id(4, 1, 0)};
    auto e2 = FootprintRouter::ring_edges({2, 2, 1});
    auto o2 = FootprintRouter::ocs_edges(e2, rm2, cm);
    EXPECT_TRUE(o2.empty());
}

TEST(FootprintRouter, AdjacentRoutesPinsAllAdjacentEdges) {
    // 2x2x1 block at the origin of an 8x8x8 torus: every ring edge (both
    // directions, including the backward edge of each 2-ring) is
    // torus-adjacent -> all of them get a single-hop pinned route over the
    // direct link.
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    std::vector<int> rank_map = {id(0, 0, 0), id(1, 0, 0), id(0, 1, 0),
                                 id(1, 1, 0)};
    auto edges = FootprintRouter::ring_edges({2, 2, 1});
    auto rows = FootprintRouter::adjacent_routes(edges, rank_map, {8, 8, 8});
    ASSERT_EQ(rows.size(), edges.size());
    for (const auto& r : rows) {
        EXPECT_EQ(r.hops.size(), 2u);
        EXPECT_EQ(r.hops.front(), r.src);
        EXPECT_EQ(r.hops.back(), r.dst);
    }
}

TEST(FootprintRouter, AdjacentRoutesSkipsNonAdjacentClosure) {
    // Shape 4x1x1 on x=0..3 of an 8-axis: the six neighbor edges (both
    // directions) are pinned 1-hop; the ring closure 3<->0 is NOT
    // torus-adjacent and must be left unpinned so it follows the backend's
    // standard DOR route like any other traffic.
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    std::vector<int> rank_map = {id(0, 0, 0), id(1, 0, 0), id(2, 0, 0),
                                 id(3, 0, 0)};
    auto edges = FootprintRouter::ring_edges({4, 1, 1});
    auto rows = FootprintRouter::adjacent_routes(edges, rank_map, {8, 8, 8});
    EXPECT_EQ(rows.size(), 6u);  // 8 directed edges - 2 closure directions
    for (const auto& r : rows) {
        EXPECT_EQ(r.hops.size(), 2u);
        // Neither closure direction may appear.
        EXPECT_FALSE(r.src == id(3, 0, 0) && r.dst == id(0, 0, 0));
        EXPECT_FALSE(r.src == id(0, 0, 0) && r.dst == id(3, 0, 0));
    }
}

TEST(FootprintRouter, AdjacentRoutesFullAxisRingClosesViaWrap) {
    // Shape 8x1x1 spanning the full x-axis: the closure 7<->0 IS
    // torus-adjacent via the wrap seam, so every directed edge is pinned
    // single-hop.
    auto id = [](int x, int y, int z) { return z * 64 + y * 8 + x; };
    std::vector<int> rank_map(8);
    for (int x = 0; x < 8; ++x) {
        rank_map[x] = id(x, 0, 0);
    }
    auto edges = FootprintRouter::ring_edges({8, 1, 1});
    auto rows = FootprintRouter::adjacent_routes(edges, rank_map, {8, 8, 8});
    EXPECT_EQ(rows.size(), edges.size());
    for (const auto& r : rows) {
        EXPECT_EQ(r.hops.size(), 2u) << r.src << "->" << r.dst;
    }
}
