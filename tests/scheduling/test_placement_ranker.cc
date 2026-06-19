/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/PlacementRanker.hh"

#include <gtest/gtest.h>

using namespace AstraSim::Scheduling;

namespace {
Placement P(bool identity,
            bool ring_closes,
            bool scattered,
            int blocks,
            int dor,
            double cost) {
    Placement p{};
    p.identity = identity;
    p.ring_closes = ring_closes;
    p.scattered = scattered;
    p.blocks_touched = blocks;
    p.dor_edges = dor;
    p.cost = cost;
    return p;
}
}  // namespace

// v2 (spec 2026-06-11 §3): identity is demoted from absolute-first. It no
// longer beats a better-packed, higher-fidelity fold "despite worse
// everything": the closed fold (class 0, dor 0) outranks an identity that
// rides DOR (class 1, dor 2) and touches more blocks.
TEST(CommFirst, IdentityNoLongerBeatsClosedFoldDespiteWorseEverything) {
    CommFirst r;
    auto ident = P(true, false, false, /*blocks=*/8, /*dor=*/2, /*cost=*/9.0);
    auto fold = P(false, true, false, /*blocks=*/1, /*dor=*/0, /*cost=*/0.0);
    EXPECT_TRUE(r.better(fold, ident));
    EXPECT_FALSE(r.better(ident, fold));
}

TEST(CommFirst, ClosedFoldBeatsOpenFold) {
    CommFirst r;
    auto closed = P(false, true, false, 4, 0, 5.0);
    auto open = P(false, false, false, 1, 1, 0.0);
    EXPECT_TRUE(r.better(closed, open));
}

TEST(CommFirst, ClassIsRealizedFidelityNotVariantGeometry) {
    CommFirst r;
    // A ring-closing variant whose closure edges could not be realized
    // (dor > 0) is class 2; ring_closes itself is ignored by this ranker.
    auto closing_unrealized = P(false, true, false, 1, 1, 0.0);
    auto open_realized = P(false, false, false, 2, 0, 5.0);
    EXPECT_TRUE(r.better(open_realized, closing_unrealized));
}

TEST(CommFirst, OpenFoldBeatsScatterEvenWhenScatterFullyClosed) {
    CommFirst r;
    auto open = P(false, false, false, 4, 3, 9.0);
    auto scatter = P(false, true, true, 1, 0, 0.0);
    EXPECT_TRUE(r.better(open, scatter));
}

// v2 (spec 2026-06-11 §3): within the open class the key is
// (dor_edges, blocks_touched, ocs_links, residual_frag, cost) — dor leads,
// not blocks. (v1 led with blocks here.)
TEST(CommFirst, WithinOpenClassDorThenPackingThenCost) {
    CommFirst r;
    // Same class (open fold): fewer dor edges wins, even with more blocks.
    EXPECT_TRUE(r.better(P(false, false, false, 2, 1, 0.0),
                         P(false, false, false, 1, 5, 9.0)));
    // Equal dor: fewer blocks wins.
    EXPECT_TRUE(r.better(P(false, false, false, 1, 2, 9.0),
                         P(false, false, false, 2, 2, 0.0)));
    // Equal dor and blocks: lower cost wins.
    EXPECT_TRUE(r.better(P(false, false, false, 2, 2, 1.0),
                         P(false, false, false, 2, 2, 2.0)));
}

TEST(CommFirst, ScatteredIdentityIsStillScatter) {
    CommFirst r;
    // MinReconfig's scatter path can tile even the identity footprint:
    // `scattered` dominates the classification (spec section 3.1).
    auto scattered_ident = P(true, true, true, 1, 0, 0.0);
    auto open_fold = P(false, false, false, 8, 9, 9.0);
    EXPECT_TRUE(r.better(open_fold, scattered_ident));
}

TEST(PackingFirst, PackingDominatesIdentity) {
    PackingFirst r;
    auto fold = P(false, true, false, 1, 0, 5.0);
    auto ident = P(true, true, false, 2, 0, 0.0);
    EXPECT_TRUE(r.better(fold, ident));
}

TEST(PackingFirst, HalfPointOpenVariantTiebreakContiguousOnly) {
    PackingFirst r;
    // Contiguous open variant pays +0.5: closed cost 1.4 beats open cost 1.0.
    auto open_contig = P(false, false, false, 1, 0, 1.0);
    auto closed_contig = P(false, true, false, 1, 0, 1.4);
    EXPECT_TRUE(r.better(closed_contig, open_contig));
    // Scattered candidates never pay it (bit-compat with the old +0.5 hack,
    // which lived in ContiguousFirst only): scatter cost 1.4 beats the
    // contiguous open candidate's effective 1.5.
    auto open_scatter = P(false, false, true, 1, 0, 1.4);
    EXPECT_TRUE(r.better(open_scatter, open_contig));
}

TEST(PlacementRanker, StrictWeakOrderingNeverSelfBetter) {
    CommFirst cf;
    PackingFirst pf;
    auto a = P(false, true, false, 2, 1, 1.5);
    auto b = P(false, true, false, 2, 1, 1.5);  // equal keys
    EXPECT_FALSE(cf.better(a, a));
    EXPECT_FALSE(pf.better(a, a));
    EXPECT_FALSE(cf.better(a, b));
    EXPECT_FALSE(cf.better(b, a));
    EXPECT_FALSE(pf.better(a, b));
    EXPECT_FALSE(pf.better(b, a));
}

TEST(PlacementRankerFactory, KnownAndUnknown) {
    EXPECT_NE(make_placement_ranker("comm-first"), nullptr);
    EXPECT_NE(make_placement_ranker("packing-first"), nullptr);
    EXPECT_EQ(make_placement_ranker("nope"), nullptr);
}

namespace {
Placement mk(int blocks,
             int dor,
             int ocs,
             int residual,
             double cost,
             bool identity,
             bool scattered) {
    Placement p;
    p.blocks_touched = blocks;
    p.dor_edges = dor;
    for (int i = 0; i < ocs; ++i) {
        p.ocs_edges.push_back({2 * i, 2 * i + 1});
    }
    p.residual_frag = residual;
    p.cost = cost;
    p.identity = identity;
    p.scattered = scattered;
    return p;
}
}  // namespace

// v2: identity is no longer its own class — a better-packed fully-closed
// fold beats an identity strip (comm-identical in-sim, better packing).
TEST(CommFirstV2, PackedClosedFoldBeatsIdentityStrip) {
    CommFirst r;
    auto fold = mk(2, 0, 1, 0, 0.0, false, false);
    auto identity = mk(3, 0, 0, 0, 0.0, true, false);
    EXPECT_TRUE(r.better(fold, identity));
}

// Identity wins class-0 ties at equal packing via ocs_links == 0.
TEST(CommFirstV2, IdentityWinsTiesViaZeroOcs) {
    CommFirst r;
    auto identity = mk(2, 0, 0, 0, 0.0, true, false);
    auto fold = mk(2, 0, 1, 0, 0.0, false, false);
    EXPECT_TRUE(r.better(identity, fold));
}

// Class order is hard: full fidelity < open < scattered, regardless of keys.
TEST(CommFirstV2, ClassOrderDominates) {
    CommFirst r;
    auto full_wide = mk(8, 0, 4, 99, 9e9, false, false);
    auto open_tight = mk(1, 1, 0, 0, 0.0, false, false);
    auto scat_perfect = mk(1, 0, 0, 0, 0.0, false, true);
    EXPECT_TRUE(r.better(full_wide, open_tight));
    EXPECT_TRUE(r.better(open_tight, scat_perfect));
}

// Wall-hugging: equal blocks/ocs => lower residual_frag wins, and the key
// sits BEFORE cost (crumb's worse cost must not matter).
TEST(CommFirstV2, ResidualFragBreaksTies) {
    CommFirst r;
    auto crumb = mk(2, 0, 1, 3, 5.0, false, false);
    auto middle = mk(2, 0, 1, 11, 0.0, false, false);
    EXPECT_TRUE(r.better(crumb, middle));
}

// dor_edges leads inside the open class.
TEST(CommFirstV2, DorLeadsOpenClass) {
    CommFirst r;
    auto one_seam = mk(9, 1, 0, 0, 9.0, false, false);
    auto two_seams = mk(1, 2, 0, 0, 0.0, false, false);
    EXPECT_TRUE(r.better(one_seam, two_seams));
}

// Ablation arms.
TEST(CommFirstV2, AblationVariants) {
    auto ocs_first = make_placement_ranker("comm-first-ocs-first");
    auto no_resid = make_placement_ranker("comm-first-no-residual");
    auto legacy = make_placement_ranker("comm-first-legacy");
    ASSERT_TRUE(ocs_first && no_resid && legacy);
    auto a = mk(2, 0, 0, 0, 0.0, false, false);  // ocs 0, blocks 2
    auto b = mk(1, 0, 3, 0, 0.0, false, false);  // ocs 3, blocks 1
    EXPECT_TRUE(ocs_first->better(a, b));        // ocs before blocks
    EXPECT_TRUE(make_placement_ranker("comm-first")->better(b, a));
    auto c = mk(2, 0, 1, 50, 0.0, false, false);
    auto d = mk(2, 0, 1, 0, 1.0, false, false);
    EXPECT_TRUE(no_resid->better(c, d));  // residual ignored => cost
    auto e = mk(2, 0, 5, 50, 0.0, false, false);
    auto f = mk(2, 0, 0, 0, 1.0, false, false);
    EXPECT_TRUE(legacy->better(e, f));  // (blocks, cost) only
}

// Q=0: only C_comm + C_rcfg matter => comm-first-like choice.
TEST(CostModel, EmptyQueueReducesToCommLike) {
    PlacementConfig cfg;
    auto r = make_placement_ranker("cost-model", cfg);
    ASSERT_TRUE(r);
    SchedContext ctx;
    ctx.queue_depth = 0;
    ctx.current_est_duration_s = 10.0;
    ctx.current_total_ring_edges = 6;
    r->set_context(&ctx);
    auto closed_wide = mk(4, 0, 2, 8, 0.0, false, false);  // rcfg only: 0.02
    auto open_tight = mk(1, 3, 0, 0, 0.0, false, false);   // comm: 1*3*10/6=5
    EXPECT_TRUE(r->better(closed_wide, open_tight));
    r->set_context(nullptr);
}

// Deep queue: the externality term dominates => packing-first-like choice.
TEST(CostModel, DeepQueueFlipsToPackingLike) {
    PlacementConfig cfg;
    auto r = make_placement_ranker("cost-model", cfg);
    SchedContext ctx;
    ctx.queue_depth = 20;
    for (int i = 0; i < 20; ++i) {
        ctx.queued.push_back({{2, 2, 2}, 8, 10.0});  // t_svc mean = 10s
    }
    ctx.current_est_duration_s = 10.0;
    ctx.current_total_ring_edges = 6;
    r->set_context(&ctx);
    // dfrag = 12/(6*2) = 1.0 => ext = 1*20*10*1 = 200s  vs  comm 5s
    auto closed_fragmenting = mk(2, 0, 0, 12, 0.0, false, false);
    auto open_wallhug = mk(2, 3, 0, 0, 0.0, false, false);
    EXPECT_TRUE(r->better(open_wallhug, closed_fragmenting));
    r->set_context(nullptr);
}

// Guard on (default): scatter never beats contiguous, whatever the scalars.
TEST(CostModel, ScatterGuardIsHard) {
    PlacementConfig cfg;
    auto on = make_placement_ranker("cost-model", cfg);
    cfg.cm_scatter_guard = false;
    auto off = make_placement_ranker("cost-model", cfg);
    SchedContext ctx;  // empty queue: costs are rcfg-only
    on->set_context(&ctx);
    off->set_context(&ctx);
    auto contiguous_pricey = mk(9, 5, 9, 0, 0.0, false, false);
    auto scatter_free = mk(1, 0, 0, 0, 0.0, false, true);
    EXPECT_TRUE(on->better(contiguous_pricey, scatter_free));
    EXPECT_TRUE(on->scatter_never_beats_closed());
    EXPECT_TRUE(off->better(scatter_free, contiguous_pricey));
    EXPECT_FALSE(off->scatter_never_beats_closed());
    on->set_context(nullptr);
    off->set_context(nullptr);
}

// DynamicSwitch: comm-first when idle, packing-first when backlogged.
TEST(DynamicSwitch, DelegatesByQueueDepth) {
    PlacementConfig cfg;  // switch_theta = 1
    auto r = make_placement_ranker("switch", cfg);
    ASSERT_TRUE(r);
    auto closed_two_blocks = mk(2, 0, 0, 0, 0.0, false, false);
    auto open_one_block = mk(1, 2, 0, 0, 0.0, false, false);
    SchedContext idle;  // depth 0 -> comm-first: class 0 beats class 1
    r->set_context(&idle);
    EXPECT_TRUE(r->better(closed_two_blocks, open_one_block));
    SchedContext busy;
    busy.queue_depth = 2;  // >= theta -> packing-first: 1 block wins
    r->set_context(&busy);
    EXPECT_TRUE(r->better(open_one_block, closed_two_blocks));
    r->set_context(nullptr);
}

// Pin the formula structure, not just orderings. The externality term uses
// the MEAN queued service time (a sum would scale C_ext as Q^2 and silently
// break the load-interpolation thesis), and C_rcfg charges exactly
// t_reconfig_s per OCS link.
TEST(CostModel, CostOfPinsMeanTsvcAndRcfgScale) {
    PlacementConfig cfg;  // kappa=1, c_ext=1, t_reconfig=0.010
    CostModelRanker r(cfg);
    SchedContext ctx;
    ctx.queue_depth = 4;
    // Mixed durations: mean = (2+4+6+8)/4 = 5s; a sum-bug would yield 20s.
    ctx.queued = {{{2, 2, 2}, 8, 2.0},
                  {{2, 2, 2}, 8, 4.0},
                  {{2, 2, 2}, 8, 6.0},
                  {{2, 2, 2}, 8, 8.0}};
    ctx.current_est_duration_s = 0.0;  // no comm term
    ctx.current_total_ring_edges = 6;
    r.set_context(&ctx);
    // dfrag = 6/(6*1) = 1.0 => C_ext = 1 * 4 * 5 * 1 = 20s; C_rcfg = 0.030.
    auto p = mk(1, 0, 3, 6, 0.0, false, false);
    EXPECT_DOUBLE_EQ(r.cost_of(p), 20.0 + 3 * 0.010);
    r.set_context(nullptr);
    // Without context: only C_rcfg remains.
    EXPECT_DOUBLE_EQ(r.cost_of(p), 3 * 0.010);
}

// Two contiguous candidates differing ONLY in ocs_links: the reconfiguration
// charge must decide (pins that C_rcfg is present and lower-is-better).
TEST(CostModel, RcfgTermBreaksTies) {
    PlacementConfig cfg;
    auto r = make_placement_ranker("cost-model", cfg);
    SchedContext ctx;  // empty queue
    r->set_context(&ctx);
    auto few_links = mk(2, 0, 1, 0, 0.0, false, false);
    auto many_links = mk(2, 0, 5, 0, 0.0, false, false);
    EXPECT_TRUE(r->better(few_links, many_links));
    r->set_context(nullptr);
}

// Boundary: depth == theta flips to packing-first (>= semantics).
TEST(DynamicSwitch, FlipsExactlyAtTheta) {
    PlacementConfig cfg;  // switch_theta = 1
    auto r = make_placement_ranker("switch", cfg);
    auto closed_two_blocks = mk(2, 0, 0, 0, 0.0, false, false);
    auto open_one_block = mk(1, 2, 0, 0, 0.0, false, false);
    SchedContext at_theta;
    at_theta.queue_depth = 1;  // == theta -> already packing-first
    r->set_context(&at_theta);
    EXPECT_TRUE(r->better(open_one_block, closed_two_blocks));
    r->set_context(nullptr);
}

// Frozen beta ("cost-model-static" arm): with cm_beta_const_s > 0 the
// externality is state-independent — charged even at Q=0, replacing the
// dynamic c_ext*Q*mean(t_svc) coefficient.
TEST(CostModel, StaticBetaFreezesExternality) {
    PlacementConfig cfg;
    cfg.cm_beta_const_s = 7.0;
    CostModelRanker r(cfg);
    SchedContext ctx;  // empty queue: dynamic beta would be 0
    r.set_context(&ctx);
    // dfrag = 6/(6*1) = 1.0 => C_ext = 7.0; C_rcfg = 0.
    auto p = mk(1, 0, 0, 6, 0.0, false, false);
    EXPECT_DOUBLE_EQ(r.cost_of(p), 7.0);
    r.set_context(nullptr);
    EXPECT_DOUBLE_EQ(r.cost_of(p), 7.0);  // truly context-independent
}
