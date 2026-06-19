/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
// The old standalone "folding" policy's behaviors, re-asserted through RFold
// at --block-size = the whole torus: a single block makes every OCS edge
// unrealizable, so the policy structurally cannot reconfigure.
#include "astra-sim/scheduling/RFold.hh"

#include "astra-sim/scheduling/BlockSelector.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/FragmentationScorer.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/PlacementPolicy.hh"
#include "astra-sim/scheduling/PlacementRanker.hh"

#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <memory>
#include <numeric>
#include <set>
#include <vector>

namespace {
using namespace AstraSim::Scheduling;
inline int nid(int x, int y, int z, int Dx, int Dy) {
    return z * Dy * Dx + y * Dx + x;
}
ClusterView full(int Dx, int Dy, int Dz) {
    std::vector<int> f(Dx * Dy * Dz);
    std::iota(f.begin(), f.end(), 0);
    return ClusterView(std::move(f), {Dx, Dy, Dz}, Dx * Dy * Dz, 0);
}
JobInstance job(int id, int K, std::array<int, 3> s) {
    JobArrival a{id, 0, K, s};
    return JobInstance(a, "", nullptr);
}
// Whole-torus rfold: block == torus dims, comm-first (the new default).
std::unique_ptr<RFold> make_wt(std::array<int, 3> dims,
                               const std::string& ranking = "comm-first") {
    return std::make_unique<RFold>(
        dims, make_block_selector("min-reconfig"),
        make_fragmentation_scorer("fewest-blocks-ocs", dims),
        make_placement_ranker(ranking));
}
// Sorted per-axis extents of a placement's coordinates on torus dims D.
std::array<int, 3> extents(const std::vector<int>& npus, std::array<int, 3> D) {
    std::array<int, 3> lo{D[0], D[1], D[2]};
    std::array<int, 3> hi{0, 0, 0};
    for (int n : npus) {
        std::array<int, 3> c = {n % D[0], (n / D[0]) % D[1], n / (D[0] * D[1])};
        for (int i = 0; i < 3; ++i) {
            lo[i] = std::min(lo[i], c[i]);
            hi[i] = std::max(hi[i], c[i]);
        }
    }
    std::array<int, 3> e = {hi[0] - lo[0] + 1, hi[1] - lo[1] + 1,
                            hi[2] - lo[2] + 1};
    std::sort(e.begin(), e.end());
    return e;
}
}  // namespace

// comm-first v2 (spec 2026-06-11 section 3): identity is demoted. The
// length-4 identity strip (1x1x4) does NOT wrap on the size-8 torus -- its
// closure has a gap of 3 and rides DOR, making it class 1 (open). The 2x2
// hairpin closes every edge in 1-hop links (class 0), so it now wins on the
// hard class order. (v1 classed identity absolute-first and kept the strip;
// the old comment's "full-axis wrap" premise was false for a length-4 ring.)
TEST(RFoldWholeTorus, IdentityDemotedFoldsTheLengthFourRing) {
    auto p = make_wt({8, 8, 8});
    auto v = full(8, 8, 8);
    auto r = p->try_place(job(1, 4, {4, 1, 1}), v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_EQ(extents(r.npus, {8, 8, 8}), (std::array<int, 3>{1, 2, 2}));
    EXPECT_TRUE(r.reconfig_plan.ocs_edges.empty());
}

// packing-first (the legacy knob) also folds the length-4 ring to the 2x2
// hairpin: equal packing (one block either way), and the hairpin has
// dor_edges 0 vs the identity strip's open partial-axis closure. Under
// comm-first v2 the two rankers now agree here (identity demotion); this
// pins that packing-first reaches the same fold via its packing key.
TEST(RFoldWholeTorus, PackingFirstFoldsTheLengthFourRing) {
    auto p = make_wt({8, 8, 8}, "packing-first");
    auto v = full(8, 8, 8);
    auto r = p->try_place(job(2, 4, {4, 1, 1}), v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_EQ(extents(r.npus, {8, 8, 8}), (std::array<int, 3>{1, 2, 2}));
}

// Ported from the old test_folding.cc: free space is only a 2x4x8 box; the
// 1x8x8 job must FOLD to 2x4x8 to fit (FirstFit would DEFER).
TEST(RFoldWholeTorus, FoldsToFitWhereFirstFitWouldDefer) {
    const int Dx = 8, Dy = 8, Dz = 8;
    std::vector<int> free;
    for (int z = 0; z < 8; ++z) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 2; ++x) {
                free.push_back(nid(x, y, z, Dx, Dy));
            }
        }
    }
    ClusterView v(free, {Dx, Dy, Dz}, Dx * Dy * Dz, 0);
    auto p = make_wt({8, 8, 8});
    auto r = p->try_place(job(3, 64, {1, 8, 8}), v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    std::set<int> fs(free.begin(), free.end());
    for (int n : r.npus) {
        EXPECT_TRUE(fs.count(n)) << n;
    }
    std::set<int> uniq(r.npus.begin(), r.npus.end());
    EXPECT_EQ(uniq.size(), 64u);
    EXPECT_TRUE(r.reconfig_plan.ocs_edges.empty());
}

// 14 = 2*7 has no fold footprint that fits a 4^3 torus (2x7 needs an axis of
// 7). With the contiguous selector there is no scatter path, so only RFold's
// own hoisted 1-D snake fallback can place the ring as a 1-hop cycle. (With
// min-reconfig the identity footprint also snakes inside scatter_assign's
// fast path; using contiguous makes the hoisted fallback load-bearing.)
TEST(RFoldWholeTorus, OneDSnakesWhenNoFoldFits) {
    auto p = std::make_unique<RFold>(
        std::array<int, 3>{4, 4, 4}, make_block_selector("contiguous"),
        make_fragmentation_scorer("fewest-blocks-ocs", {4, 4, 4}),
        make_placement_ranker("comm-first"));
    auto v = full(4, 4, 4);
    auto r = p->try_place(job(4, 14, {14, 1, 1}), v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_TRUE(r.ordered_rings);
    std::set<int> uniq(r.npus.begin(), r.npus.end());
    EXPECT_EQ(uniq.size(), 14u);
    EXPECT_TRUE(r.reconfig_plan.ocs_edges.empty());
    // 14 undirected ring edges, both directions pinned as 1-hop routes --
    // fails if the snake ever stops being a true 1-hop cycle.
    EXPECT_EQ(r.reconfig_plan.routes.size(), 28u);
}

// A length-13 ring needs an odd 1-hop cycle, which a bipartite (all-even
// dims) torus cannot host -- but a 1-D job small enough for the cluster must
// DEFER, never DROP (the snake might fit after the cluster changes).
TEST(RFoldWholeTorus, OneDDefersNeverDrops) {
    auto p = make_wt({4, 4, 4});
    auto v = full(4, 4, 4);
    EXPECT_EQ(p->try_place(job(5, 13, {13, 1, 1}), v).outcome,
              PlacementOutcome::DEFER);
}

// The case that emits OCS at block 4x4x4 (see test_rfold.cc
// EmitsOcsEdgesForBlockSelfWrap) must emit none at whole-torus block: the
// only realizable face pairs are full-axis wraps, which are already torus
// links.
TEST(RFoldWholeTorus, NeverEmitsOcsEdges) {
    auto p = make_wt({8, 8, 8});
    auto v = full(8, 8, 8);
    auto r = p->try_place(job(6, 16, {4, 4, 1}), v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_TRUE(r.reconfig_plan.ocs_edges.empty());
    EXPECT_FALSE(r.reconfig_plan.routes.empty());
}

TEST(RFoldWholeTorus, DropWhenImpossible) {
    auto p = make_wt({4, 4, 4});
    auto v = full(4, 4, 4);
    EXPECT_EQ(p->try_place(job(7, 100, {5, 5, 4}), v).outcome,
              PlacementOutcome::DROP);
}

// A 2-D job (two non-unit axes) gets no snake fallback: when no fold
// footprint fits the cluster dims it must DROP, not DEFER. Pins the
// non_unit <= 1 gate -- widening it flips this to DEFER (the odd 25-cycle
// cannot close on a bipartite torus, but any_fits would be forced true).
TEST(RFoldWholeTorus, TwoDNoFitDropsNotDefers) {
    auto p = make_wt({4, 4, 4});
    auto v = full(4, 4, 4);
    EXPECT_EQ(p->try_place(job(10, 25, {5, 5, 1}), v).outcome,
              PlacementOutcome::DROP);
}

// Multi-fold closures at whole-torus block are realizable only as full-axis
// wraps (already torus links), so even deep folds wire NO OCS edges:
// folding-mode semantics survive the generalization.
TEST(RFoldWholeTorus, MultiFoldWiresNoOcs) {
    auto p = make_wt({8, 8, 8});
    auto v = full(8, 8, 8);
    auto j = job(20, 64, {32, 2, 1});
    auto r = p->try_place(j, v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_TRUE(r.reconfig_plan.ocs_edges.empty());
}

TEST(RFoldWholeTorus, DeferWhenNoFreeNow3D) {
    std::vector<int> free = {0, 1, 2, 3, 4, 5, 6};  // 7 < 8 ranks
    ClusterView v(free, {4, 4, 4}, 64, 0);
    auto p = make_wt({4, 4, 4});
    EXPECT_EQ(p->try_place(job(8, 8, {2, 2, 2}), v).outcome,
              PlacementOutcome::DEFER);
}

// Deliberate drift from the old folding policy, which classed candidates by
// variant geometry and picked the compact 4x4x4 cube via scorer cost. The
// comm-first default classes by REALIZED fidelity: the 2x2x16 strip runs
// every ring edge over a 1-hop link (class 1) while the straddling cube
// leaves closure edges on multi-hop DOR (class 2), so the strip wins even
// though it is less compact.
TEST(RFoldWholeTorus, CommFirstPicksFullFidelityOverCompactness) {
    const int Dx = 16, Dy = 16;
    std::vector<int> free;
    std::set<int> strip;
    for (int z = 0; z < 16; ++z) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                int n = nid(x, y, z, Dx, Dy);
                free.push_back(n);
                strip.insert(n);
            }
        }
    }
    for (int z = 2; z < 6; ++z) {
        for (int y = 2; y < 6; ++y) {
            for (int x = 2; x < 6; ++x) {
                free.push_back(nid(x, y, z, Dx, Dy));
            }
        }
    }
    ClusterView v(free, {16, 16, 16}, 4096, 0);
    PlacementConfig cfg;
    cfg.block_size = {16, 16, 16};  // whole torus
    auto p = make_placement_policy("rfold", cfg);
    ASSERT_NE(p, nullptr);
    auto r = p->try_place(job(9, 64, {2, 4, 8}), v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_EQ(extents(r.npus, {16, 16, 16}), (std::array<int, 3>{2, 2, 16}));
    for (int n : r.npus) {
        EXPECT_TRUE(strip.count(n) > 0)
            << "node " << n << " outside the full-fidelity 2x2x16 strip";
    }
    EXPECT_TRUE(r.reconfig_plan.ocs_edges.empty());
}
