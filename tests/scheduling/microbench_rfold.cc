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
// (docs/rdcn-model.md): fragmented cubes = cubes a placement occupies
// partially; heuristic changes (candidate ordering, cube sharing, the
// ranking key) should tighten the EXPECT bounds scenario
// by scenario.
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
    BlockModel cm{kDims, kBlock, /*ocs_enabled=*/true};

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
// trace, and the one where the tiling never divides cleanly. Five jobs
// placed back-to-back measure the whole ranking's fragmentation economy;
// under the old contiguity-tiered ranking the sequence fragmented 20 cubes,
// under frag-first it fragments 11 (floor 5).
TEST(Microbench, Frag72FamilySequence) {
    Driver d;
    int total_frag = 0;
    for (int i = 0; i < 5; ++i) {
        auto rep = d.place(job(i, 72, {2, 6, 6}));
        ASSERT_GE(rep.cubes_touched, 0) << "job " << i << " failed to place";
        EXPECT_LE(rep.frag_cubes, 4);  // contiguous ceil-box bound
        total_frag += rep.frag_cubes;
    }
    std::printf("[metric] 72-chip x5: total fragmented cubes = %d "
                "(floor 5)\n",
                total_frag);
}

// Non-contiguous placement for the 72-chip family: only two far-apart idle
// cubes are free, so no solid box fits anywhere.
//
// Under the old all-edges-must-wire veto this DEFERRED forever: the veto
// restricted non-contiguous side lengths to {1, 2, 4, 8}, so any dim with a
// factor of 3 (the whole 72-chip family, ~10% of pareto128 chip-mass and
// much more of pareto256/512) could not place non-contiguously at all.
// Best-effort wiring unlocks it: unwirable fold turns and ring closures
// ride the shared fabric and are priced as multihop.
TEST(Microbench, NonContiguous72Places) {
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
    ASSERT_GE(rep.cubes_touched, 0) << "non-contiguous placement failed";
    EXPECT_EQ(rep.cubes_touched, 2);
    EXPECT_LE(rep.frag_cubes, 2);
    std::printf("[metric] non-contiguous 72: cubes=%d frag=%d wired=%d "
                "multihop=%d\n",
                rep.cubes_touched, rep.frag_cubes, rep.wired, rep.multihop);
}

// Cube sharing across sequential jobs: three idle cubes, two 72-chip jobs
// (144 chips). Without cube sharing each job needs 2 fresh cubes -> 4
// cubes, so the second job cannot fit. With it, job B packs its tiles into
// job A's leftovers: both jobs fit in 3 cubes with zero wasted chips.
TEST(Microbench, TwoJobCubeSharing) {
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
        << "job B failed — cube sharing should fit both jobs in 3 cubes";
    std::printf("[metric] two-job cube sharing: free left=%zu (of 192)\n",
                d.free.size());
    EXPECT_EQ(d.free.size(), 192u - 144u);
}

// Whole-cube placement across far cubes: only cubes (0,0,0) and (1,1,1)
// are free — no solid 128-chip box exists anywhere. Under the RDCN model
// the two far cubes are as good as adjacent ones: the job must place,
// fully wired, with ZERO fragmented cubes and zero multihop edges.
TEST(Microbench, FarCubeWholeCubePlacement) {
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
    ASSERT_GE(rep.cubes_touched, 0) << "far-cube placement failed";
    EXPECT_EQ(rep.frag_cubes, 0);
    EXPECT_EQ(rep.multihop, 0);
    std::printf(
        "[metric] far-cube placement: wired=%d default_circuit=%d intra=%d\n",
        rep.wired, rep.default_seam, rep.intra_cube);
}

// Cross-job fragmentation concentration: job A fragments cubes; an
// identical job B arrives. The fragmented-cubes-first candidate order packs
// B's partial tiles into A's fragmented cubes (chips permitting) instead of
// breaking into fresh idle ones.
TEST(Microbench, CrossJobFragConcentration) {
    Driver d;
    auto a = d.place(job(0, 72, {2, 6, 6}));
    ASSERT_GE(a.cubes_touched, 0);
    // Record cubes A fragmented (partially occupied now).
    std::set<std::array<int, 3>> frag_before;
    for (int n = 0; n < 512; ++n) {
        if (d.free.count(n) == 0) {
            frag_before.insert(d.cm.block_of(n));
        }
    }
    auto b = d.place(job(1, 72, {2, 6, 6}));
    ASSERT_GE(b.cubes_touched, 0);
    // How many fresh idle cubes did B break into?
    std::set<std::array<int, 3>> b_cubes;
    for (int n = 0; n < 512; ++n) {
        if (d.free.count(n) == 0) {
            b_cubes.insert(d.cm.block_of(n));
        }
    }
    int fresh = static_cast<int>(b_cubes.size() - frag_before.size());
    std::printf(
        "[metric] cross-job: A touched %d cubes, B broke %d fresh cubes\n",
        static_cast<int>(frag_before.size()), fresh);
    EXPECT_LE(fresh, 1);  // fragmented-cubes-first keeps B out of idle cubes
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
        EXPECT_LE(rep.frag_cubes, c.dirty_bound);
        std::printf("[metric] %dx%dx%d: cubes=%d frag=%d multihop=%d\n", c.s[0],
                    c.s[1], c.s[2], rep.cubes_touched, rep.frag_cubes,
                    rep.multihop);
    }
}
