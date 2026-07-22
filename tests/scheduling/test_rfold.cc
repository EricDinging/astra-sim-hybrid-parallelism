/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/RFold.hh"

#include "astra-sim/scheduling/BlockSelector.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/FootprintRouter.hh"
#include "astra-sim/scheduling/FragmentationScorer.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/PlacementPolicy.hh"
#include "astra-sim/scheduling/PlacementRanker.hh"
#include "astra-sim/scheduling/SchedContext.hh"

#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <memory>
#include <numeric>
#include <unordered_set>
#include <vector>

namespace {
using namespace AstraSim::Scheduling;
ClusterView full(int Dx, int Dy, int Dz) {
    std::vector<int> f(Dx * Dy * Dz);
    std::iota(f.begin(), f.end(), 0);
    return ClusterView(std::move(f), {Dx, Dy, Dz}, Dx * Dy * Dz, 0);
}
JobInstance job(int id, int K, std::array<int, 3> s) {
    JobArrival a{id, 0, K, s};
    return JobInstance(a, "", nullptr);
}
std::unique_ptr<RFold> make(std::array<int, 3> block = {4, 4, 4}) {
    return std::make_unique<RFold>(
        block, make_block_selector("contiguous"),
        make_fragmentation_scorer("fewest-blocks", {4, 4, 4}),
        make_placement_ranker("comm-first"));
}
}  // namespace

TEST(RFold, PlacesAndSetsOrderedRings) {
    auto p = make();
    auto v = full(8, 8, 8);
    auto j = job(1, 8, {2, 2, 2});
    auto r = p->try_place(j, v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_TRUE(r.ordered_rings);
    EXPECT_EQ(r.npus.size(), 8u);
    EXPECT_TRUE(r.reconfig_plan.ocs_edges.empty());  // fits one block
    EXPECT_FALSE(r.reconfig_plan.routes.empty());    // ring sends have routes
}

TEST(RFold, EmitsOcsEdgesForBlockSelfWrap) {
    auto p = make();
    auto v = full(8, 8, 8);
    // 4x4x1 has length-4 rings that can't all fold to an adjacent closure
    // within one block, so the (realizable) placement wires block self-wrap OCS
    // links: opposite faces of a block along the ring axis.
    auto j = job(2, 16, {4, 4, 1});
    auto r = p->try_place(j, v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_FALSE(r.reconfig_plan.ocs_edges.empty());
}

TEST(RFold, DropWhenImpossible) {
    auto p = make();
    auto v = full(4, 4, 4);
    auto j = job(3, 100, {5, 5, 4});  // 100 > 64 total
    EXPECT_EQ(p->try_place(j, v).outcome, PlacementOutcome::DROP);
}

TEST(RFold, DeferWhenNoFreeNow) {
    std::vector<int> free = {0, 1, 2, 3, 4, 5, 6};  // 7 < 8 ranks
    ClusterView v(free, {4, 4, 4}, 64, 0);
    auto j = job(4, 8, {2, 2, 2});
    auto p = make();
    EXPECT_EQ(p->try_place(j, v).outcome, PlacementOutcome::DEFER);
}

TEST(RFold, DropWhenBlockSizeDoesNotDivide) {
    auto p = make(/*block=*/{3, 3, 3});  // 3 does not divide 8
    auto v = full(8, 8, 8);
    auto j = job(5, 8, {2, 2, 2});
    EXPECT_EQ(p->try_place(j, v).outcome, PlacementOutcome::DROP);
}

namespace {
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

// Identity demotion (comm-first, spec 2026-06-11 section 3). For an 8-ring
// at block 4x4x4 the identity strip (1x1x8) is class 0 -- its full-axis wrap
// closes on a torus link -- but it straddles two blocks. comm-first judges it
// on realized fidelity (class 0, like the folds) and then by packing, so the
// class-0 tiebreak (blocks_touched after dor) picks the single-block 2x2x2
// cube fold. packing-first folds for the same packing reason, so the two
// rankers agree here.
TEST(RFold, IdentityDemotedFoldsTheEightRing) {
    auto v = full(8, 8, 8);
    auto j = job(6, 8, {8, 1, 1});

    auto comm = std::make_unique<RFold>(
        std::array<int, 3>{4, 4, 4}, make_block_selector("contiguous"),
        make_fragmentation_scorer("fewest-blocks", {4, 4, 4}),
        make_placement_ranker("comm-first"));
    auto rc = comm->try_place(j, v);
    ASSERT_EQ(rc.outcome, PlacementOutcome::PLACED);
    // Folded into a single 4x4x4 block: a 2x2x2 cube, largest extent <= 4.
    EXPECT_LE(extents(rc.npus, {8, 8, 8})[2], 4);

    auto packing = std::make_unique<RFold>(
        std::array<int, 3>{4, 4, 4}, make_block_selector("contiguous"),
        make_fragmentation_scorer("fewest-blocks", {4, 4, 4}),
        make_placement_ranker("packing-first"));
    auto rp = packing->try_place(j, v);
    ASSERT_EQ(rp.outcome, PlacementOutcome::PLACED);
    EXPECT_LE(extents(rp.npus, {8, 8, 8})[2], 4);
}

// Scatter-only shapes (no cuboid fold variant fits the torus dims, e.g.
// 32x2x1 on 8x8x8) must DEFER, not DROP, when the cluster is merely busy:
// DROP is reserved for shapes that cannot place even on an idle torus.
TEST(RFold, ScatterOnlyShapeDefersWhenBusy) {
    auto p = make_placement_policy("rfold", PlacementConfig{});
    auto j = job(7, 64, {32, 2, 1});
    // idle torus: scatter places it
    EXPECT_EQ(p->try_place(j, full(8, 8, 8)).outcome, PlacementOutcome::PLACED);
    // busy torus (too few free NPUs): same shape must wait, not drop
    std::vector<int> few(32);
    std::iota(few.begin(), few.end(), 0);
    ClusterView busy(few, {8, 8, 8}, 512, 0);
    EXPECT_EQ(p->try_place(j, busy).outcome, PlacementOutcome::DEFER);
}

// The idle-torus oracle under DOR-tolerant glue (RDCN, 2026-07-21): 16x16x2
// IS placeable on an idle torus — its (16,8,4) fold variant tiles into
// exactly 8 full cubes, glued with the unwirable ring edges riding the
// shared fabric — so rfold defers while waiting for space.
TEST(RFold, GluePlaceableShapeDefersWhenBusy) {
    auto j = job(8, 512, {16, 16, 2});
    std::vector<int> few(32);
    std::iota(few.begin(), few.end(), 0);
    ClusterView busy(few, {8, 8, 8}, 512, 0);
    auto p = make_placement_policy("rfold", PlacementConfig{});
    EXPECT_EQ(p->try_place(j, busy).outcome, PlacementOutcome::DEFER);
}

// Under permanent NPU failures "idle" means the degraded torus. On 4x4x4
// every {4,4,2} placement spans two full adjacent slabs along some axis;
// failing (0,0,0) and (2,2,2) puts a failed node in every such slab pair,
// so the shape can never place again even though its footprint fits the
// torus dims (the footprint-fit check alone would DEFER it to sim exit).
TEST(RFold, DegradedIdleUnplaceableShapeDrops) {
    auto p = make();  // block 4x4x4 == whole torus on 4^3: no OCS scatter
    auto j = job(9, 32, {4, 4, 2});
    std::unordered_set<int> failed = {0, 42};  // (0,0,0) and (2,2,2)
    std::vector<int> free;
    for (int i = 0; i < 64; ++i) {
        if (failed.find(i) == failed.end()) {
            free.push_back(i);
        }
    }
    ClusterView idle_degraded(std::move(free), {4, 4, 4}, 62, 0, failed);
    EXPECT_EQ(p->try_place(j, idle_degraded).outcome, PlacementOutcome::DROP);
}

// A shape that still fits the degraded torus keeps DEFERring while busy.
TEST(RFold, DegradedPlaceableShapeDefersWhenBusy) {
    auto p = make();
    auto j = job(10, 8, {2, 2, 2});
    std::unordered_set<int> failed = {0, 42};
    ClusterView busy({}, {4, 4, 4}, 62, 0, failed);  // nothing free right now
    EXPECT_EQ(p->try_place(j, busy).outcome, PlacementOutcome::DEFER);
}

TEST(RFoldFactory, DefaultConfigBuilds) {
    EXPECT_NE(make_placement_policy("rfold", PlacementConfig{}), nullptr);
}

TEST(RFoldFactory, UnknownRankingRejected) {
    PlacementConfig cfg;
    cfg.rfold_ranking = "nope";
    EXPECT_EQ(make_placement_policy("rfold", cfg), nullptr);
}

TEST(RFoldFactory, FoldingNameRemoved) {
    EXPECT_EQ(make_placement_policy("folding", PlacementConfig{}), nullptr);
}

// 96x1x1 used to be a 200k-expansion snake gamble; multi-fold places it as a
// fully-1-hop closing variant (the snake fallback only runs when no variant
// places). WHICH variant wins is a ranking choice — an exact 3x4x8 box and a
// 96-node serpentine inside a 4x4x8 box tie on every comm-first key (dor 0,
// 2 blocks, equal cost), and ties resolve by scan order — so the pinned
// invariant is: compact contiguous placement (bounding volume within one
// block pair, not a sprawling snake) + full comm fidelity (every ring edge
// pinned 1-hop).
TEST(RFoldMultiFold, Ring96PlacesContiguouslyOnIdle8x8x8) {
    auto p = make();
    auto v = full(8, 8, 8);
    auto j = job(10, 96, {96, 1, 1});
    auto r = p->try_place(j, v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    auto ext = extents(r.npus, {8, 8, 8});
    EXPECT_LE(ext[0] * ext[1] * ext[2], 128);  // compact variant, not a snake
    const auto ring = FootprintRouter::ring_edges({96, 1, 1});
    EXPECT_EQ(r.reconfig_plan.routes.size(), ring.size());
}

// 2-D job needing a deep fold: every 2-part factor tuple of 32 has a factor
// >= 16 that overflows the size-8 torus axis, so dim0 must fold deeper
// (e.g. (8,4) across two axes). Unreachable legacy (one hairpin per dim).
TEST(RFoldMultiFold, TwoDeeJobNeedingDeepFoldPlaces) {
    auto p = make();
    auto v = full(8, 8, 8);
    auto j = job(11, 64, {32, 2, 1});
    auto r = p->try_place(j, v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
}

// A serpentine closure that does NOT span a full torus axis needs the OCS.
// Confine a 16x2x1 job to one block's 4x4x2 corner: dim0 is forced into the
// (4,4) tuple (2x8/8x2 overflow the region; identity 16 overflows; 3-part
// tuples leave no axis for dim1). Its closure gap of 3 lands on opposite
// faces of the single block => self-wrap OCS links; full-axis-wrap variants
// cannot fit, so the OCS path is the only closing option.
TEST(RFoldMultiFold, SubAxisClosureWiredByOcsAtBlock4) {
    auto p = make();  // block 4x4x4, contiguous selector
    std::vector<int> free;
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                free.push_back(z * 64 + y * 8 + x);
            }
        }
    }
    ClusterView v(free, {8, 8, 8}, 512, 0);
    auto j = job(12, 32, {16, 2, 1});
    auto r = p->try_place(j, v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_FALSE(r.reconfig_plan.ocs_edges.empty());
    const auto ring = FootprintRouter::ring_edges({16, 2, 1});
    EXPECT_EQ(r.reconfig_plan.routes.size(), ring.size());
}

// Odd 1-D longer than every axis can never place (prime: no factor tuple
// fits, no closed odd cycle exists in the bipartite torus), so the idle
// oracle DROPs it. Used to DEFER forever under the old 1-D any_fits
// shortcut, which skipped the oracle and aborted the sim at drain.
TEST(RFoldMultiFold, OddLongRingDrops) {
    auto p = make();
    auto v = full(8, 8, 8);
    auto j = job(13, 11, {11, 1, 1});
    EXPECT_EQ(p->try_place(j, v).outcome, PlacementOutcome::DROP);
}

// Even 1-D whose length has no <=axis factor tuple (262 = 2*131) and whose
// snake DFS exhausts kSnakeBudget even on an IDLE torus: the oracle mirrors
// the snake failure, so the job DROPs instead of deferring forever.
TEST(RFoldMultiFold, SnakeBudgetFailureOnIdleDrops) {
    auto p = make();
    auto v = full(8, 8, 8);
    auto j = job(14, 262, {1, 1, 262});
    EXPECT_EQ(p->try_place(j, v).outcome, PlacementOutcome::DROP);
}

// The oracle consult must not turn busy-cluster defers into drops: 1x1x22
// (22 = 2*11, no cuboid variant) snakes fine on an idle torus but cannot
// close a cycle through pairwise-nonadjacent free nodes -- DEFER, retry
// when the free set changes.
TEST(RFoldMultiFold, SnakeableRingDefersWhenBusy) {
    auto p = make();
    std::vector<int> free;  // even coords only: no two nodes torus-adjacent
    for (int z = 0; z < 8; z += 2) {
        for (int y = 0; y < 8; y += 2) {
            for (int x = 0; x < 8; x += 2) {
                free.push_back(z * 64 + y * 8 + x);
            }
        }
    }
    ClusterView busy(std::move(free), {8, 8, 8}, 512, 0);
    auto j = job(15, 22, {1, 1, 22});
    EXPECT_EQ(p->try_place(j, busy).outcome, PlacementOutcome::DEFER);
}

// And the snake success path is untouched: 1x1x22 on an idle torus places.
TEST(RFoldMultiFold, SnakeableRingPlacesWhenIdle) {
    auto p = make();
    auto v = full(8, 8, 8);
    auto j = job(16, 22, {1, 1, 22});
    EXPECT_EQ(p->try_place(j, v).outcome, PlacementOutcome::PLACED);
}

namespace {
// Records the context the policy installs on the ranker.
struct RecordingRanker : public CommFirst {
    int last_depth = -1;
    bool cleared = false;
    void set_context(const SchedContext* ctx) override {
        if (ctx == nullptr) {
            cleared = true;
            return;
        }
        cleared = false;
        last_depth = ctx->queue_depth;
    }
};
}  // namespace

TEST(RFoldSchedContext, InstalledAndClearedAroundTryPlace) {
    auto rec = std::make_unique<RecordingRanker>();
    auto* rec_raw = rec.get();
    RFold p({4, 4, 4}, make_block_selector("contiguous"),
            make_fragmentation_scorer("fewest-blocks", {4, 4, 4}),
            std::move(rec));
    SchedContext ctx;
    ctx.queue_depth = 3;
    p.set_sched_context(&ctx);
    auto v = full(8, 8, 8);
    auto j = job(30, 8, {2, 2, 2});
    auto r = p.try_place(j, v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_EQ(rec_raw->last_depth, 3);
    EXPECT_TRUE(rec_raw->cleared);  // dangling-pointer guard ran
}
