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

// Spec 2026-06-11 §3: identity is demoted from absolute-first. It no
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

// Within the open class the key is (dor_edges, blocks_touched, cost) — dor
// leads, not blocks.
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

// frag-first: no contiguity tier — a tiled placement that fragments fewer
// idle cubes beats a solid box that fragments more, and tiled candidates are
// always searched (scatter_never_beats_closed false).
TEST(FragFirst, FragmentationOutranksContiguity) {
    auto r = make_placement_ranker("frag-first");
    ASSERT_TRUE(r);
    EXPECT_FALSE(r->scatter_never_beats_closed());
    auto box = P(false, true, false, /*blocks=*/4, /*dor=*/0, /*cost=*/0.0);
    box.frag_delta = 4;
    auto tiled = P(false, true, true, /*blocks=*/2, /*dor=*/9, /*cost=*/9.0);
    tiled.frag_delta = 2;
    EXPECT_TRUE(r->better(tiled, box));
    // Equal fragmentation: fewer cubes, then fewer multi-hop edges.
    auto a = P(false, true, true, 2, 3, 0.0);
    auto b = P(false, true, false, 2, 1, 0.0);
    EXPECT_TRUE(r->better(b, a));
}

namespace {
Placement mk(int blocks, int dor, double cost, bool identity, bool scattered) {
    Placement p;
    p.blocks_touched = blocks;
    p.dor_edges = dor;
    p.cost = cost;
    p.identity = identity;
    p.scattered = scattered;
    return p;
}
}  // namespace

// Identity is not its own class — a better-packed fully-closed fold beats
// an identity strip (comm-identical in-sim, better packing).
TEST(CommFirst, PackedClosedFoldBeatsIdentityStrip) {
    CommFirst r;
    auto fold = mk(2, 0, 0.0, false, false);
    auto identity = mk(3, 0, 0.0, true, false);
    EXPECT_TRUE(r.better(fold, identity));
}

// Class order is hard: full fidelity < open < scattered, regardless of keys.
TEST(CommFirst, ClassOrderDominates) {
    CommFirst r;
    auto full_wide = mk(8, 0, 9e9, false, false);
    auto open_tight = mk(1, 1, 0.0, false, false);
    auto scat_perfect = mk(1, 0, 0.0, false, true);
    EXPECT_TRUE(r.better(full_wide, open_tight));
    EXPECT_TRUE(r.better(open_tight, scat_perfect));
}

// dor_edges leads inside the open class.
TEST(CommFirst, DorLeadsOpenClass) {
    CommFirst r;
    auto one_seam = mk(9, 1, 9.0, false, false);
    auto two_seams = mk(1, 2, 0.0, false, false);
    EXPECT_TRUE(r.better(one_seam, two_seams));
}

// Scatter-class key order is (frag_delta, blocks_touched, dor_edges, cost):
// newly-fragmented blocks lead, each later key only breaks earlier ties.
TEST(CommFirst, ScatterClassLeadsWithDirtyDelta) {
    CommFirst r;
    auto clean = mk(9, 9, 9.0, false, true);
    clean.frag_delta = 0;
    auto dirty = mk(1, 0, 0.0, false, true);
    dirty.frag_delta = 1;
    EXPECT_TRUE(r.better(clean, dirty));  // frag_delta beats all later keys
    // Equal frag_delta: fewer blocks, then fewer dor edges, then cost.
    auto a = mk(1, 5, 9.0, false, true);
    auto b = mk(2, 0, 0.0, false, true);
    EXPECT_TRUE(r.better(a, b));
    auto c = mk(2, 1, 9.0, false, true);
    auto d = mk(2, 2, 0.0, false, true);
    EXPECT_TRUE(r.better(c, d));
    auto e = mk(2, 2, 1.0, false, true);
    auto f = mk(2, 2, 2.0, false, true);
    EXPECT_TRUE(r.better(e, f));
}

// DynamicSwitch: comm-first when idle, packing-first when backlogged.
TEST(DynamicSwitch, DelegatesByQueueDepth) {
    PlacementConfig cfg;  // switch_theta = 1
    auto r = make_placement_ranker("switch", cfg);
    ASSERT_TRUE(r);
    auto closed_two_blocks = mk(2, 0, 0.0, false, false);
    auto open_one_block = mk(1, 2, 0.0, false, false);
    SchedContext idle;  // depth 0 -> comm-first: class 0 beats class 1
    r->set_context(&idle);
    EXPECT_TRUE(r->better(closed_two_blocks, open_one_block));
    SchedContext busy;
    busy.queue_depth = 2;  // >= theta -> packing-first: 1 block wins
    r->set_context(&busy);
    EXPECT_TRUE(r->better(open_one_block, closed_two_blocks));
    r->set_context(nullptr);
}

// Boundary: depth == theta flips to packing-first (>= semantics).
TEST(DynamicSwitch, FlipsExactlyAtTheta) {
    PlacementConfig cfg;  // switch_theta = 1
    auto r = make_placement_ranker("switch", cfg);
    auto closed_two_blocks = mk(2, 0, 0.0, false, false);
    auto open_one_block = mk(1, 2, 0.0, false, false);
    SchedContext at_theta;
    at_theta.queue_depth = 1;  // == theta -> already packing-first
    r->set_context(&at_theta);
    EXPECT_TRUE(r->better(open_one_block, closed_two_blocks));
    r->set_context(nullptr);
}
