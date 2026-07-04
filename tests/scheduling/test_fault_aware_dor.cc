/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

// Unit tests for fault-aware DOR. A 4x4x4 torus is built directly; routes are
// computed on demand by the Router and read via get_precomputed_route()
// (set_failed_npus() clears the router cache, so failures take effect on the
// next lookup). The DOR + fault-detour logic lives in Router::compute_dor().

#include <astra-network-analytical/common/EventQueue.h>
#include <astra-network-analytical/reconfigurable/TopologyManager.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <unordered_set>
#include <vector>

using NetworkAnalytical::EventQueue;
using NetworkAnalyticalReconfigurable::TopologyManager;

namespace {

constexpr int Dx = 4, Dy = 4, Dz = 4, N = Dx * Dy * Dz;

std::array<int, 3> to_coord(int id) {
    return {id % Dx, (id / Dx) % Dy, id / (Dx * Dy)};
}
int to_id(int x, int y, int z) {
    return z * Dy * Dx + y * Dx + x;
}

// Reference baseline DOR: X then Y then Z, unidirectional (+1) torus arc.
std::vector<int> expected_dor(int s, int t) {
    auto c = to_coord(s);
    const auto d = to_coord(t);
    const int Nv[3] = {Dx, Dy, Dz};
    std::vector<int> ids = {s};
    for (int dim = 0; dim < 3; ++dim) {
        int hops = ((d[dim] - c[dim]) + Nv[dim]) % Nv[dim];
        for (int h = 0; h < hops; ++h) {
            c[dim] = (c[dim] + 1) % Nv[dim];
            ids.push_back(to_id(c[0], c[1], c[2]));
        }
    }
    return ids;
}

std::vector<int> route_ids(TopologyManager& tm, int s, int t) {
    std::vector<int> ids;
    for (const auto& dev : tm.get_precomputed_route(s, t)) {
        ids.push_back(dev->get_id());
    }
    return ids;
}

// Each consecutive pair differs by exactly 1 in exactly one dim (mod N): a real
// torus neighbor hop.
bool all_neighbor_hops(const std::vector<int>& ids) {
    const int Nv[3] = {Dx, Dy, Dz};
    for (std::size_t i = 1; i < ids.size(); ++i) {
        auto a = to_coord(ids[i - 1]);
        auto b = to_coord(ids[i]);
        int diffs = 0;
        for (int dim = 0; dim < 3; ++dim) {
            int delta = (a[dim] - b[dim] + Nv[dim]) % Nv[dim];
            if (delta != 0) {
                ++diffs;
                if (delta != 1 && delta != Nv[dim] - 1) {
                    return false;
                }
            }
        }
        if (diffs != 1) {
            return false;
        }
    }
    return true;
}

TopologyManager make_tm(EventQueue* eq) {
    return TopologyManager(N, N, eq, /*bw_schedules=*/{},
                           /*latency_schedules=*/{},
                           /*npus_per_dim=*/{Dx, Dy, Dz},
                           /*is_torus=*/true);
}

}  // namespace

// With no failures, fault-aware DOR == baseline DOR for every pair.
TEST(FaultAwareDor, NoFailureEquivalence) {
    EventQueue eq;
    TopologyManager tm = make_tm(&eq);
    for (int s = 0; s < N; ++s) {
        for (int t = 0; t < N; ++t) {
            EXPECT_EQ(route_ids(tm, s, t), expected_dor(s, t))
                << "s=" << s << " t=" << t;
        }
    }
}

// A straight-line pair whose DOR path crosses a failed node detours around it.
TEST(FaultAwareDor, DetoursAroundFailedNode) {
    EventQueue eq;
    TopologyManager tm = make_tm(&eq);
    tm.set_failed_npus({to_id(1, 0, 0)});  // sits on 0 -> 2 DOR path

    const int s = to_id(0, 0, 0), t = to_id(2, 0, 0);
    std::vector<int> r = route_ids(tm, s, t);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.front(), s);
    EXPECT_EQ(r.back(), t);
    EXPECT_EQ(std::count(r.begin(), r.end(), to_id(1, 0, 0)), 0);  // avoided
    EXPECT_TRUE(all_neighbor_hops(r));
    EXPECT_GT(r.size(), expected_dor(s, t).size());  // detour is longer
}

// A pair whose DOR path avoids all failed nodes is unchanged.
TEST(FaultAwareDor, UnaffectedPairUnchanged) {
    EventQueue eq;
    TopologyManager tm = make_tm(&eq);
    tm.set_failed_npus({to_id(1, 0, 0)});
    // 0 -> (0,1,0): DOR moves only in Y, never touches (1,0,0).
    const int s = to_id(0, 0, 0), t = to_id(0, 1, 0);
    EXPECT_EQ(route_ids(tm, s, t), expected_dor(s, t));
}

// A pair whose source or destination is itself failed gets the trivial 2-node
// stub route {endpoint, other}: failed nodes never send/recv, so the table
// entry is never queried, and the walk is skipped (no spurious hard-fail).
TEST(FaultAwareDor, FailedEndpointStub) {
    EventQueue eq;
    TopologyManager tm = make_tm(&eq);
    const int f = to_id(1, 0, 0);
    tm.set_failed_npus({f});

    // Failed source: route(f, t) -> {f, t}.
    const int t = to_id(2, 2, 2);
    std::vector<int> rs = route_ids(tm, f, t);
    ASSERT_EQ(rs.size(), 2u);
    EXPECT_EQ(rs.front(), f);
    EXPECT_EQ(rs.back(), t);

    // Failed destination: route(s, f) -> {s, f}.
    const int s = to_id(3, 3, 3);
    std::vector<int> rd = route_ids(tm, s, f);
    ASSERT_EQ(rd.size(), 2u);
    EXPECT_EQ(rd.front(), s);
    EXPECT_EQ(rd.back(), f);
}

// When a node is boxed in (all six neighbors failed), the first DOR lookup that
// must leave it hard-fails with exit code 1. Use a 3x3x3 torus and fail all
// neighbors of node 0.
TEST(FaultAwareDorDeath, HardFailsWhenBoxedIn) {
    using NetworkAnalyticalReconfigurable::TopologyManager;
    EventQueue eq;
    const int dx = 3, dy = 3, dz = 3, n = dx * dy * dz;
    TopologyManager tm(n, n, &eq, {}, {}, {dx, dy, dz}, /*is_torus=*/true);
    // Neighbors of (0,0,0) on a 3-torus: x=+/-1 -> ids 1,2; y -> 3,6; z ->
    // 9,18.
    tm.set_failed_npus({1, 2, 3, 6, 9, 18});
    // Routing 0 -> 13 (=(1,1,1)) must leave node 0, but every exit neighbor is
    // failed; the on-demand DOR walk finds no detour and exits(1).
    EXPECT_EXIT(route_ids(tm, 0, 13), ::testing::ExitedWithCode(1),
                "no .*detour");
}
