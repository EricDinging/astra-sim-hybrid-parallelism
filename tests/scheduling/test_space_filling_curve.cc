/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

// Unit tests for HilbertCurve (math) and SpaceFillingCurve (placement policy).
// Parity literals were generated with hilbertcurve==2.0.5 (Python).

#include "astra-sim/scheduling/HilbertCurve.hh"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <numeric>
#include <set>
#include <vector>

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/PlacementPolicy.hh"
#include "astra-sim/scheduling/SpaceFillingCurve.hh"

namespace {

using AstraSim::Scheduling::hilbert_d_from_xyz;
using AstraSim::Scheduling::hilbert_xyz_from_d;

using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::PlacementOutcome;
using AstraSim::Scheduling::PlacementResult;
using AstraSim::Scheduling::SpaceFillingCurve;

JobInstance make_job(int id, int A, int B, int C) {
    JobArrival arr{};
    arr.job_id = id;
    arr.arrival_time = 0;
    arr.num_ranks = A * B * C;
    arr.shape = {A, B, C};
    return JobInstance(arr, /*trace_dir=*/"", /*runtime=*/nullptr);
}

ClusterView make_view(int W, int L, int H, const std::vector<int>& free_ids) {
    return ClusterView(free_ids, /*physical_dims=*/{W, L, H},
                       /*total_npus=*/W * L * H, /*current_tick=*/0);
}

std::vector<int> all_ids(int total) {
    std::vector<int> v(total);
    std::iota(v.begin(), v.end(), 0);
    return v;
}

}  // namespace

TEST(HilbertCurve, ZeroDistanceIsOrigin) {
    EXPECT_EQ(hilbert_d_from_xyz(0, 0, 0, 2), 0u);
    auto p = hilbert_xyz_from_d(0, 2);
    EXPECT_EQ(p[0], 0);
    EXPECT_EQ(p[1], 0);
    EXPECT_EQ(p[2], 0);
}

// Known (coord, distance) pairs generated from
// hilbertcurve==2.0.5 with p=2, n=3. See doc/superpowers/plans/
// 2026-05-25-sfc-placement.md for the exact derivation.
TEST(HilbertCurve, KnownValuesP2) {
    struct KV {
        int x, y, z;
        uint64_t d;
    };
    const KV kv[] = {
        {0, 0, 0, 0}, {0, 1, 0, 1},  {1, 0, 0, 3},  {1, 1, 1, 5},
        {0, 0, 1, 7}, {2, 2, 2, 40}, {3, 3, 3, 45},
    };
    for (const auto& k : kv) {
        EXPECT_EQ(hilbert_d_from_xyz(k.x, k.y, k.z, 2), k.d)
            << "coord (" << k.x << "," << k.y << "," << k.z << ")";
        auto pt = hilbert_xyz_from_d(k.d, 2);
        EXPECT_EQ(pt[0], k.x) << "d=" << k.d;
        EXPECT_EQ(pt[1], k.y) << "d=" << k.d;
        EXPECT_EQ(pt[2], k.z) << "d=" << k.d;
    }
}

// Round-trip every coord in the 2^p cube for p in {1, 2, 3, 4} and verify
// distances are unique and cover [0, 2^(3p)).
TEST(HilbertCurve, RoundTripAndUniqueness) {
    for (int p = 1; p <= 4; ++p) {
        const int side = 1 << p;
        const uint64_t total = static_cast<uint64_t>(side) * side * side;
        std::set<uint64_t> seen;
        for (int z = 0; z < side; ++z) {
            for (int y = 0; y < side; ++y) {
                for (int x = 0; x < side; ++x) {
                    const uint64_t d = hilbert_d_from_xyz(x, y, z, p);
                    EXPECT_LT(d, total) << "p=" << p;
                    EXPECT_TRUE(seen.insert(d).second)
                        << "duplicate d=" << d << " at (" << x << "," << y
                        << "," << z << ") p=" << p;
                    auto back = hilbert_xyz_from_d(d, p);
                    EXPECT_EQ(back[0], x) << "p=" << p << " d=" << d;
                    EXPECT_EQ(back[1], y) << "p=" << p << " d=" << d;
                    EXPECT_EQ(back[2], z) << "p=" << p << " d=" << d;
                }
            }
        }
        EXPECT_EQ(seen.size(), total) << "p=" << p;
    }
}

TEST(SpaceFillingCurve, PlacesOnEmptyCube) {
    SpaceFillingCurve policy;
    auto job = make_job(/*id=*/1, /*A=*/2, /*B=*/2, /*C=*/2);
    auto view = make_view(4, 4, 4, all_ids(64));

    PlacementResult r = policy.try_place(job, view);
    EXPECT_EQ(r.outcome, PlacementOutcome::PLACED) << r.reason;
    EXPECT_EQ(static_cast<int>(r.npus.size()), 8);
}

TEST(SpaceFillingCurve, DefersWhenFreePoolTooSmall) {
    SpaceFillingCurve policy;
    auto job = make_job(/*id=*/2, 2, 2, 2);                       // needs 8
    auto view = make_view(2, 2, 2, /*free=*/{0, 1, 2, 3, 4, 5});  // only 6
    PlacementResult r = policy.try_place(job, view);
    EXPECT_EQ(r.outcome, PlacementOutcome::DEFER);
    EXPECT_EQ(r.reason, "fewer free NPUs than num_ranks");
}

TEST(SpaceFillingCurve, DropsOnNonThreeDimDims) {
    SpaceFillingCurve policy;
    auto job = make_job(/*id=*/3, 2, 1, 1);
    // 1-D cluster: ClusterView accepts whatever the runtime passes; SFC
    // rejects.
    auto view = ClusterView(/*free=*/{0, 1}, /*physical_dims=*/{64},
                            /*total_npus=*/64, /*current_tick=*/0);
    PlacementResult r = policy.try_place(job, view);
    EXPECT_EQ(r.outcome, PlacementOutcome::DROP);
    EXPECT_EQ(r.reason, "SFC requires 3-D physical_dims (--npus-per-dim)");
}

TEST(SpaceFillingCurve, DropsWhenJobLargerThanCluster) {
    SpaceFillingCurve policy;
    auto job = make_job(/*id=*/4, 3, 3, 3);      // 27 ranks
    auto view = make_view(2, 2, 2, all_ids(8));  // total 8
    PlacementResult r = policy.try_place(job, view);
    EXPECT_EQ(r.outcome, PlacementOutcome::DROP);
    EXPECT_EQ(r.reason, "num_ranks exceeds total NPUs");
}

// Parity literals generated once with hilbertcurve==2.0.5 via
// placement_lib.py::SpaceFillingCurve.allocate semantics:
//   W=L=H=4, two consecutive shape=(2,2,2) jobs on an initially-empty
//   cluster.
// job1.npus  -> [0, 4, 5, 1, 17, 21, 20, 16]
// job2.npus  -> [32, 48, 49, 33, 37, 53, 52, 36]
TEST(SpaceFillingCurve, ParityFourCubedTwoJobs) {
    SpaceFillingCurve policy;

    auto view1 = make_view(4, 4, 4, all_ids(64));
    auto job1 = make_job(/*id=*/10, 2, 2, 2);
    PlacementResult r1 = policy.try_place(job1, view1);
    ASSERT_EQ(r1.outcome, PlacementOutcome::PLACED) << r1.reason;
    const std::vector<int> expected1 = {0, 4, 5, 1, 17, 21, 20, 16};
    EXPECT_EQ(r1.npus, expected1);

    // For job2, mark job1's 8 ids occupied and place again. Since the
    // policy is stateless, just rebuild the view with the smaller free
    // set.
    std::vector<int> free2;
    free2.reserve(56);
    std::set<int> occupied(r1.npus.begin(), r1.npus.end());
    for (int id = 0; id < 64; ++id) {
        if (occupied.find(id) == occupied.end()) {
            free2.push_back(id);
        }
    }
    auto view2 = make_view(4, 4, 4, free2);
    auto job2 = make_job(/*id=*/11, 2, 2, 2);
    PlacementResult r2 = policy.try_place(job2, view2);
    ASSERT_EQ(r2.outcome, PlacementOutcome::PLACED) << r2.reason;
    const std::vector<int> expected2 = {32, 48, 49, 33, 37, 53, 52, 36};
    EXPECT_EQ(r2.npus, expected2);
}

// Parity literal generated with hilbertcurve==2.0.5:
//   W=L=16, H=8 (non-cube, embedding cube 16^3), one shape=(2,2,2) job
//   on an empty cluster.
// job.npus -> [0, 256, 272, 16, 17, 273, 257, 1]
TEST(SpaceFillingCurve, ParityNonCubeTorus) {
    SpaceFillingCurve policy;
    auto view = make_view(16, 16, 8, all_ids(16 * 16 * 8));
    auto job = make_job(/*id=*/20, 2, 2, 2);
    PlacementResult r = policy.try_place(job, view);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED) << r.reason;
    const std::vector<int> expected = {0, 256, 272, 16, 17, 273, 257, 1};
    EXPECT_EQ(r.npus, expected);
    for (int npu : r.npus) {
        EXPECT_GE(npu, 0);
        EXPECT_LT(npu, 16 * 16 * 8);
    }
}

TEST(SpaceFillingCurve, IsDeterministic) {
    SpaceFillingCurve policy;
    auto view = make_view(4, 4, 4, all_ids(64));
    auto job = make_job(/*id=*/30, 2, 2, 2);
    PlacementResult a = policy.try_place(job, view);
    PlacementResult b = policy.try_place(job, view);
    EXPECT_EQ(a.outcome, PlacementOutcome::PLACED);
    EXPECT_EQ(a.outcome, b.outcome);
    EXPECT_EQ(a.npus, b.npus);
}

TEST(SpaceFillingCurve, MakeSchedulerReturnsSfcInstance) {
    auto sched = AstraSim::Scheduling::make_placement_policy("sfc");
    ASSERT_NE(sched, nullptr);
    EXPECT_EQ(sched->name(), "sfc");
}
