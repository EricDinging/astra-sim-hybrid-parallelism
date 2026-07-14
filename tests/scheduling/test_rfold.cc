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
        make_fragmentation_scorer("fewest-blocks-ocs", {4, 4, 4}),
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

// Identity demotion (comm-first v2, spec 2026-06-11 section 3). For an 8-ring
// at block 4x4x4 the identity strip (1x1x8) is class 0 -- its full-axis wrap
// closes on a torus link -- but it straddles two blocks. v1 comm-first kept
// the strip by classing identity absolute-first; v2 judges it on realized
// fidelity (class 0, like the folds) and then by packing, so the class-0
// tiebreak (blocks_touched first) picks the single-block 2x2x2 cube fold.
// packing-first folds for the same packing reason, so the two rankers now
// agree here -- the v1 divergence was precisely the demoted identity bias.
TEST(RFold, IdentityDemotedFoldsTheEightRing) {
    auto v = full(8, 8, 8);
    auto j = job(6, 8, {8, 1, 1});

    auto comm = std::make_unique<RFold>(
        std::array<int, 3>{4, 4, 4}, make_block_selector("contiguous"),
        make_fragmentation_scorer("fewest-blocks-ocs", {4, 4, 4}),
        make_placement_ranker("comm-first"));
    auto rc = comm->try_place(j, v);
    ASSERT_EQ(rc.outcome, PlacementOutcome::PLACED);
    // Folded into a single 4x4x4 block: a 2x2x2 cube, largest extent <= 4.
    EXPECT_LE(extents(rc.npus, {8, 8, 8})[2], 4);

    auto packing = std::make_unique<RFold>(
        std::array<int, 3>{4, 4, 4}, make_block_selector("contiguous"),
        make_fragmentation_scorer("fewest-blocks-ocs", {4, 4, 4}),
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

TEST(RFold, IdleUnplaceableShapeStillDropsWhenBusy) {
    auto p = make_placement_policy("rfold", PlacementConfig{});
    auto j = job(8, 512, {16, 16, 2});  // not placeable even on an idle torus
    std::vector<int> few(32);
    std::iota(few.begin(), few.end(), 0);
    ClusterView busy(few, {8, 8, 8}, 512, 0);
    EXPECT_EQ(p->try_place(j, busy).outcome, PlacementOutcome::DROP);
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
// fully-1-hop closing box (the snake fallback only runs when no variant
// places). WHICH closing box wins is a ranking choice — e.g. 3x4x8 beats
// 2x6x8 on blocks_touched — so the pinned invariant is: exact box (bounding
// volume == ranks) + full comm fidelity (every ring edge pinned 1-hop).
TEST(RFoldMultiFold, Ring96PlacesAsBoxOnIdle8x8x8) {
    auto p = make();
    auto v = full(8, 8, 8);
    auto j = job(10, 96, {96, 1, 1});
    auto r = p->try_place(j, v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    auto ext = extents(r.npus, {8, 8, 8});
    EXPECT_EQ(ext[0] * ext[1] * ext[2], 96);  // exact box, not a snake
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
    int last_ring_edges = -1;
    bool cleared = false;
    void set_context(const SchedContext* ctx) override {
        if (ctx == nullptr) {
            cleared = true;
            return;
        }
        cleared = false;
        last_depth = ctx->queue_depth;
        last_ring_edges = ctx->current_total_ring_edges;
    }
};
}  // namespace

TEST(RFoldSchedContext, EnrichedAndClearedAroundTryPlace) {
    auto rec = std::make_unique<RecordingRanker>();
    auto* rec_raw = rec.get();
    RFold p({4, 4, 4}, make_block_selector("contiguous"),
            make_fragmentation_scorer("fewest-blocks-ocs", {4, 4, 4}),
            std::move(rec));
    SchedContext ctx;
    ctx.queue_depth = 3;
    ctx.queued = {{{2, 2, 2}, 8, 1.5}, {{4, 1, 1}, 4, 0.5}};
    p.set_sched_context(&ctx);
    auto v = full(8, 8, 8);
    auto j = job(30, 8, {2, 2, 2});
    auto r = p.try_place(j, v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_EQ(rec_raw->last_depth, 3);
    EXPECT_EQ(rec_raw->last_ring_edges,
              static_cast<int>(FootprintRouter::ring_edges({2, 2, 2}).size()));
    EXPECT_TRUE(rec_raw->cleared);  // dangling-pointer guard ran
}

TEST(RFoldLookahead, ChargesFinalistsThatBlockQueuedJobs) {
    // Two hand-built candidates; the fake probe says a queued job needs
    // NPU 0: any candidate consuming it gets charged gamma * est.
    Placement a;
    a.rank_map = {0, 1};
    Placement b;
    b.rank_map = {2, 3};
    std::vector<Placement> cands = {a, b};
    SchedContext ctx;
    ctx.queue_depth = 1;
    ctx.queued = {{{2, 1, 1}, 2, 10.0}};
    ctx.probe = [](const std::array<int, 3>&, int,
                   const std::unordered_set<int>& free) {
        return free.count(0) > 0;
    };
    std::unordered_set<int> free = {0, 1, 2, 3, 4};
    detail::apply_lookahead(cands, /*k=*/2, /*q=*/8, /*gamma=*/0.5, ctx, free);
    EXPECT_DOUBLE_EQ(cands[0].lookahead_ext_s, 5.0);  // blocks NPU 0
    EXPECT_DOUBLE_EQ(cands[1].lookahead_ext_s, 0.0);
}

TEST(RFoldLookahead, RespectsTopQBound) {
    Placement a;
    a.rank_map = {0};
    std::vector<Placement> cands = {a};
    SchedContext ctx;
    int calls = 0;
    for (int i = 0; i < 10; ++i) {
        ctx.queued.push_back({{1, 1, 1}, 1, 1.0});
    }
    ctx.queue_depth = 10;
    ctx.probe = [&calls](const std::array<int, 3>&, int,
                         const std::unordered_set<int>&) {
        ++calls;
        return true;
    };
    std::unordered_set<int> free = {0, 1};
    detail::apply_lookahead(cands, 1, 3, 1.0, ctx, free);
    EXPECT_LE(calls, 2 * 3);  // before+after per queued job, top-3 only
}

// End-to-end: with lookahead on, RFold avoids the placement that blocks the
// queued job. Probe injected via SchedContext (RFold prefers ctx.probe).
TEST(RFoldLookahead, TryPlaceAvoidsBlockingCandidate) {
    PlacementConfig cfg;
    cfg.cm_lookahead_k = 8;
    auto ranker = make_placement_ranker("cost-model", cfg);
    RFold p({4, 4, 4}, make_block_selector("contiguous"),
            make_fragmentation_scorer("fewest-blocks-ocs", {4, 4, 4}),
            std::move(ranker), cfg);
    SchedContext ctx;
    ctx.queue_depth = 1;
    ctx.queued = {{{1, 1, 8}, 8, 100.0}};
    // The queued job "needs" NPU 0 (origin cell).
    ctx.probe = [](const std::array<int, 3>&, int,
                   const std::unordered_set<int>& free) {
        return free.count(0) > 0;
    };
    p.set_sched_context(&ctx);
    auto v = full(8, 8, 8);
    auto j = job(40, 8, {1, 1, 8});
    auto r = p.try_place(j, v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    EXPECT_EQ(std::count(r.npus.begin(), r.npus.end(), 0), 0)
        << "lookahead should have steered away from NPU 0";
}

// apply_lookahead truncates to the top-k finalists (input is pre-sorted
// best-first), keeping the FRONT of the vector.
TEST(RFoldLookahead, TruncatesToTopKKeepingFront) {
    Placement a;
    a.rank_map = {0};
    Placement b;
    b.rank_map = {1};
    Placement c;
    c.rank_map = {2};
    std::vector<Placement> cands = {a, b, c};
    SchedContext ctx;
    ctx.queued = {{{1, 1, 1}, 1, 1.0}};
    ctx.queue_depth = 1;
    ctx.probe = [](const std::array<int, 3>&, int,
                   const std::unordered_set<int>&) { return true; };
    std::unordered_set<int> free = {0, 1, 2};
    detail::apply_lookahead(cands, /*k=*/1, /*q=*/8, /*gamma=*/1.0, ctx, free);
    ASSERT_EQ(cands.size(), 1u);
    EXPECT_EQ(cands[0].rank_map, std::vector<int>({0}));
}

// Default probe (no injection): free space is one z-column (8 NPUs at
// x=0,y=0) plus a 2x2x2 cube at (4..5,4..5,0..1). The queued 5-ring fits
// ONLY the column (5 is prime: no fold fits the cube). An 8-ring fits both.
// With lookahead on, the built-in enumerate+select probe must detect that
// taking the column blocks the queued job, and steer the 8-ring to the cube.
TEST(RFoldLookahead, DefaultProbeSteersAwayFromQueuedJobsOnlyHome) {
    PlacementConfig cfg;
    cfg.cm_lookahead_k = 8;
    auto ranker = make_placement_ranker("cost-model", cfg);
    RFold p({4, 4, 4}, make_block_selector("contiguous"),
            make_fragmentation_scorer("fewest-blocks-ocs", {4, 4, 4}),
            std::move(ranker), cfg);
    std::vector<int> free;
    for (int z = 0; z < 8; ++z) {
        free.push_back(z * 64);  // column x=0,y=0
    }
    for (int z = 0; z < 2; ++z) {
        for (int y = 4; y < 6; ++y) {
            for (int x = 4; x < 6; ++x) {
                free.push_back(z * 64 + y * 8 + x);  // 2x2x2 cube
            }
        }
    }
    ClusterView v(free, {8, 8, 8}, 512, 0);
    SchedContext ctx;
    ctx.queue_depth = 1;
    ctx.queued = {{{5, 1, 1}, 5, 100.0}};  // 5-ring: column-only
    p.set_sched_context(&ctx);
    auto j = job(41, 8, {8, 1, 1});
    auto r = p.try_place(j, v);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED);
    // Steered into the cube: none of the column NPUs are used.
    for (int z = 0; z < 8; ++z) {
        EXPECT_EQ(std::count(r.npus.begin(), r.npus.end(), z * 64), 0)
            << "column NPU taken at z=" << z;
    }
}
