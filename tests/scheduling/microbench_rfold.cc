/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
// RFold micro-benchmarks: heavier-than-unit-test scenarios that validate one
// placement heuristic each, without an end-to-end simulation. Every scenario
// asserts RDCN-model legality (rdcn_check) plus scenario-specific bounds,
// and prints its metrics so a heuristic's benefit is visible from the ctest
// log. Runtime budget: a couple of seconds per scenario.
//
// Metrics printed here are the analytic census's vocabulary
// (docs/rdcn-model.md): dirty cubes = cubes a placement occupies partially;
// the improvement stack (dirt-seeking cube choice, tile co-residence,
// dirty-delta ranking) should tighten the EXPECT bounds scenario by
// scenario.
#include "astra-sim/scheduling/BlockModel.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/FootprintRouter.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/PlacementPolicy.hh"
#include "rdcn_check.hh"

#include <array>
#include <cstdio>
#include <gtest/gtest.h>
#include <memory>
#include <numeric>
#include <set>
#include <unordered_set>
#include <vector>

namespace {
using namespace AstraSim::Scheduling;

constexpr std::array<int, 3> kDims = {8, 8, 8};
constexpr std::array<int, 3> kBlock = {4, 4, 4};

JobInstance job(int id, int ranks, std::array<int, 3> s) {
    JobArrival a{id, 0, ranks, s};
    return JobInstance(a, "", nullptr);
}

// Sequential placement driver: places jobs one by one on a shrinking free
// set (no completions), asserting legality per placement. Returns per-job
// RDCN reports for placed jobs; a DEFER/DROP is recorded as a report with
// cubes_touched == -1.
struct Driver {
    std::unique_ptr<PlacementPolicy> policy =
        make_placement_policy("rfold", PlacementConfig{});
    std::set<int> free;
    BlockModel cm{kDims, kBlock, /*polarity_free=*/true};

    Driver() {
        for (int i = 0; i < kDims[0] * kDims[1] * kDims[2]; ++i) {
            free.insert(i);
        }
    }
    void occupy(std::array<int, 3> lo, std::array<int, 3> hi) {
        for (int z = lo[2]; z < hi[2]; ++z) {
            for (int y = lo[1]; y < hi[1]; ++y) {
                for (int x = lo[0]; x < hi[0]; ++x) {
                    free.erase(cm.id_of({x, y, z}));
                }
            }
        }
    }
    RdcnReport place(const JobInstance& j) {
        ClusterView v(std::vector<int>(free.begin(), free.end()),
                      {kDims[0], kDims[1], kDims[2]},
                      kDims[0] * kDims[1] * kDims[2], 0);
        auto r = policy->try_place(j, v);
        if (r.outcome != PlacementOutcome::PLACED) {
            RdcnReport miss;
            miss.cubes_touched = -1;
            return miss;
        }
        const auto ring = FootprintRouter::ring_edges(j.shape);
        auto rep = rdcn_check(r, ring, cm, kBlock, kDims);
        EXPECT_TRUE(rep.ok) << rep.violation;
        for (int n : r.npus) {
            EXPECT_EQ(free.erase(n), 1u) << "placement reused a busy NPU";
        }
        return rep;
    }
};
}  // namespace

// The 72-chip family (2x6x6): the largest chip-mass family of the pareto128
// trace. On an idle torus the class gate picks a CONTIGUOUS box, which
// spreads over its ceil-box (4 cubes, all partial) — per-placement dirt the
// design accepts because contiguous dirt is reusable by the next contiguous
// neighbor. This scenario documents that behavior; the glue path's tighter
// economics are asserted in ForcedGlue72 below.
TEST(Microbench, DirtyCube72FamilyContiguous) {
    Driver d;
    int total_dirty = 0;
    for (int i = 0; i < 5; ++i) {
        auto rep = d.place(job(i, 72, {2, 6, 6}));
        ASSERT_GE(rep.cubes_touched, 0) << "job " << i << " failed to place";
        EXPECT_LE(rep.dirty_cubes, 4);  // contiguous ceil-box bound
        total_dirty += rep.dirty_cubes;
    }
    std::printf("[metric] 72-chip x5 contiguous: total dirty cubes = %d "
                "(glue floor 10, nest floor 5)\n",
                total_dirty);
}

// Forced glue for the 72-chip family: only two far-apart whole cubes are
// free, so no contiguous variant fits and the glue path must serve.
//
// Under the strict pre-RDCN gate this DEFERRED: grid gluing required every
// ring edge realizable, restricting glueable dims to {1, 2, 4, 8} — any dim
// with a factor of 3 (the whole 72-chip family, ~10% of pareto128 chip-mass
// and much more of pareto256/512) could not glue at all. DOR-tolerant glue
// (RDCN mode, 2026-07-21) unlocks it: unwirable fold turns and closures
// ride the shared fabric (S3) and are paid as dor_edges. Nesting should
// later reach dirty == 1 (the 64+8 split).
TEST(Microbench, ForcedGlue72Places) {
    Driver d;
    d.occupy({0, 0, 0}, {8, 8, 8});
    for (auto c : {std::array<int, 3>{0, 0, 0}, std::array<int, 3>{4, 4, 4}}) {
        for (int z = c[2]; z < c[2] + 4; ++z) {
            for (int y = c[1]; y < c[1] + 4; ++y) {
                for (int x = c[0]; x < c[0] + 4; ++x) {
                    d.free.insert(d.cm.id_of({x, y, z}));
                }
            }
        }
    }
    auto rep = d.place(job(0, 72, {2, 6, 6}));
    ASSERT_GE(rep.cubes_touched, 0) << "forced glue failed to place";
    EXPECT_EQ(rep.cubes_touched, 2);
    EXPECT_LE(rep.dirty_cubes, 2);  // nesting target: 1
    std::printf("[metric] forced-glue 72: cubes=%d dirty=%d wired=%d "
                "multihop=%d\n",
                rep.cubes_touched, rep.dirty_cubes, rep.wired, rep.multihop);
}

// Tile nesting across sequential jobs: three clean cubes, two 72-chip jobs
// (144 chips). Without nesting each job grid-glues into 2 fresh cubes -> 4
// cubes, 4 dirty (needs a 4th cube). With nesting job B reuses job A's
// leftovers: total 3 cubes, all dirty but none wasted — and the placement
// FITS where the unnested version cannot.
TEST(Microbench, TwoJobNestedGlue) {
    Driver d;
    d.occupy({0, 0, 0}, {8, 8, 8});
    for (auto c : {std::array<int, 3>{0, 0, 0}, std::array<int, 3>{4, 0, 0},
                   std::array<int, 3>{0, 4, 0}}) {
        for (int z = c[2]; z < c[2] + 4; ++z) {
            for (int y = c[1]; y < c[1] + 4; ++y) {
                for (int x = c[0]; x < c[0] + 4; ++x) {
                    d.free.insert(d.cm.id_of({x, y, z}));
                }
            }
        }
    }
    auto a = d.place(job(0, 72, {2, 6, 6}));
    ASSERT_GE(a.cubes_touched, 0) << "job A failed";
    auto b = d.place(job(1, 72, {2, 6, 6}));
    ASSERT_GE(b.cubes_touched, 0)
        << "job B failed — nesting should fit both jobs in 3 cubes";
    std::printf("[metric] two-job nest: free left=%zu (of 192)\n",
                d.free.size());
    EXPECT_EQ(d.free.size(), 192u - 144u);
}

// Non-adjacent whole-cube gluing: only cubes (0,0,0) and (1,1,1) are free —
// no contiguous 128-chip box exists anywhere. RDCN says the two far cubes
// are as good as adjacent ones: the job must place, fully wired, with ZERO
// dirty cubes and zero multihop edges.
TEST(Microbench, FragmentedFabricWholeCubeGlue) {
    Driver d;
    d.occupy({0, 0, 0}, {8, 8, 8});
    for (auto c : {std::array<int, 3>{0, 0, 0}, std::array<int, 3>{4, 4, 4}}) {
        for (int z = c[2]; z < c[2] + 4; ++z) {
            for (int y = c[1]; y < c[1] + 4; ++y) {
                for (int x = c[0]; x < c[0] + 4; ++x) {
                    d.free.insert(d.cm.id_of({x, y, z}));
                }
            }
        }
    }
    auto rep = d.place(job(0, 128, {8, 4, 4}));
    ASSERT_GE(rep.cubes_touched, 0) << "glue across far cubes failed";
    EXPECT_EQ(rep.dirty_cubes, 0);
    EXPECT_EQ(rep.multihop, 0);
    std::printf("[metric] far-cube glue: wired=%d default_seam=%d intra=%d\n",
                rep.wired, rep.default_seam, rep.intra_cube);
}

// Cross-job dirt concentration: job A dirties a cube; an identical job B
// arrives. Ideal cube choice nests B's partial tiles into A's dirty cubes
// (chips permitting) instead of wounding fresh ones. Prints the reuse count;
// bound is loose until dirt-seeking cube choice lands.
TEST(Microbench, CornerCollisionCrossJob) {
    Driver d;
    auto a = d.place(job(0, 72, {2, 6, 6}));
    ASSERT_GE(a.cubes_touched, 0);
    // Record cubes dirtied by A (partially occupied now).
    std::set<std::array<int, 3>> dirty_before;
    for (int n = 0; n < 512; ++n) {
        if (d.free.count(n) == 0) {
            dirty_before.insert(d.cm.block_of(n));
        }
    }
    auto b = d.place(job(1, 72, {2, 6, 6}));
    ASSERT_GE(b.cubes_touched, 0);
    // How many cubes did B touch that A had already wounded?
    std::set<std::array<int, 3>> b_cubes;
    for (int n = 0; n < 512; ++n) {
        if (d.free.count(n) == 0) {
            b_cubes.insert(d.cm.block_of(n));
        }
    }
    int fresh = static_cast<int>(b_cubes.size() - dirty_before.size());
    std::printf(
        "[metric] cross-job: A touched %d cubes, B wounded %d fresh cubes\n",
        static_cast<int>(dirty_before.size()), fresh);
    EXPECT_LE(fresh, 4);  // loose today; tighten with dirt-seeking choice
}

// Workload generality (user 2026-07-21): the heuristics must help larger
// shapes too (pareto256/512, uniform512 draw them). Big awkward shapes place
// legally on an idle torus with bounded dirt.
TEST(Microbench, BigShapeGenerality) {
    struct Case {
        std::array<int, 3> s;
        int ranks;
        int dirty_bound;  // contiguous ceil-box bound today; nesting floor 1
    };
    for (const auto& c : {Case{{4, 6, 8}, 192, 2}, Case{{6, 6, 8}, 288, 6},
                          Case{{6, 8, 8}, 384, 4}, Case{{2, 6, 8}, 96, 4}}) {
        Driver d;
        auto rep = d.place(job(0, c.ranks, c.s));
        ASSERT_GE(rep.cubes_touched, 0)
            << c.s[0] << "x" << c.s[1] << "x" << c.s[2] << " failed";
        EXPECT_LE(rep.dirty_cubes, c.dirty_bound);
        std::printf("[metric] %dx%dx%d: cubes=%d dirty=%d multihop=%d\n",
                    c.s[0], c.s[1], c.s[2], rep.cubes_touched, rep.dirty_cubes,
                    rep.multihop);
    }
}
