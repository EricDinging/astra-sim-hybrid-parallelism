/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/L1Clustering.hh"

#include <gtest/gtest.h>

#include <numeric>
#include <set>
#include <vector>

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/PlacementPolicy.hh"

namespace {

using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::L1Clustering;
using AstraSim::Scheduling::PlacementOutcome;
using AstraSim::Scheduling::PlacementResult;

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

// --- Error conditions ---

TEST(L1Clustering, DropsOnNonThreeDimDims) {
    L1Clustering policy;
    auto job = make_job(/*id=*/1, 2, 1, 1);
    auto view = ClusterView(/*free=*/{0, 1}, /*physical_dims=*/{64},
                            /*total_npus=*/64, /*current_tick=*/0);
    PlacementResult r = policy.try_place(job, view);
    EXPECT_EQ(r.outcome, PlacementOutcome::DROP);
    EXPECT_EQ(r.reason,
              "L1Clustering requires 3-D physical_dims (--npus-per-dim)");
}

TEST(L1Clustering, DropsWhenJobLargerThanCluster) {
    L1Clustering policy;
    auto job = make_job(/*id=*/2, 3, 3, 3);      // 27 ranks
    auto view = make_view(2, 2, 2, all_ids(8));  // total 8
    PlacementResult r = policy.try_place(job, view);
    EXPECT_EQ(r.outcome, PlacementOutcome::DROP);
    EXPECT_EQ(r.reason, "num_ranks exceeds total NPUs");
}

TEST(L1Clustering, DefersWhenFreePoolTooSmall) {
    L1Clustering policy;
    auto job = make_job(/*id=*/3, 2, 2, 2);                       // needs 8
    auto view = make_view(2, 2, 2, /*free=*/{0, 1, 2, 3, 4, 5});  // only 6
    PlacementResult r = policy.try_place(job, view);
    EXPECT_EQ(r.outcome, PlacementOutcome::DEFER);
    EXPECT_EQ(r.reason, "fewer free NPUs than num_ranks");
}

// --- Parity tests ---

// 4^3 torus, all 64 free, job needs 8. step=max(1,64/100)=1 (exhaustive).
// Every center has cost 8 (torus is vertex-transitive); center 0 wins by
// tiebreaker. Its 8 closest by (dist,id): {0}@0, {1,3,4,12,16,48}@1, {2}@2.
TEST(L1Clustering, ParityFourCubedAllFree) {
    L1Clustering policy;
    auto view = make_view(4, 4, 4, all_ids(64));
    auto job = make_job(/*id=*/10, 2, 2, 2);
    PlacementResult r = policy.try_place(job, view);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED) << r.reason;
    const std::vector<int> expected = {0, 1, 3, 4, 12, 16, 48, 2};
    EXPECT_EQ(r.npus, expected);
}

// 4^3 torus, 10 free NPUs (2x2x2 cube at origin + (0,0,2) + (3,3,3)),
// job needs 8. Center 16 (0,0,1) wins with cost 10 — pulls in nearby
// NPU 32 at distance 1, beating center 0 (cost 11).
TEST(L1Clustering, ParityAsymmetricFreeSet) {
    L1Clustering policy;
    const std::vector<int> free = {0, 1, 4, 5, 16, 17, 20, 21, 32, 63};
    auto view = make_view(4, 4, 4, free);
    auto job = make_job(/*id=*/11, 2, 2, 2);
    PlacementResult r = policy.try_place(job, view);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED) << r.reason;
    const std::vector<int> expected = {16, 0, 17, 20, 32, 1, 4, 21};
    EXPECT_EQ(r.npus, expected);
}

// --- Behavioral tests ---

TEST(L1Clustering, IsDeterministic) {
    L1Clustering policy;
    auto view = make_view(4, 4, 4, all_ids(64));
    auto job = make_job(/*id=*/20, 2, 2, 2);
    PlacementResult a = policy.try_place(job, view);
    PlacementResult b = policy.try_place(job, view);
    EXPECT_EQ(a.outcome, PlacementOutcome::PLACED);
    EXPECT_EQ(a.outcome, b.outcome);
    EXPECT_EQ(a.npus, b.npus);
}

TEST(L1Clustering, PlacedNpusAreValidAndUnique) {
    L1Clustering policy;
    auto view = make_view(4, 4, 4, all_ids(64));
    auto job = make_job(/*id=*/21, 4, 2, 2);  // 16 ranks
    PlacementResult r = policy.try_place(job, view);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED) << r.reason;
    ASSERT_EQ(static_cast<int>(r.npus.size()), 16);
    std::set<int> unique(r.npus.begin(), r.npus.end());
    EXPECT_EQ(unique.size(), r.npus.size());
    for (int npu : r.npus) {
        EXPECT_GE(npu, 0);
        EXPECT_LT(npu, 64);
    }
}

TEST(L1Clustering, PlacedNpusAreSubsetOfFree) {
    L1Clustering policy;
    const std::vector<int> free = {0, 1, 4, 5, 16, 17, 20, 21, 32, 63};
    auto view = make_view(4, 4, 4, free);
    auto job = make_job(/*id=*/22, 2, 2, 2);
    PlacementResult r = policy.try_place(job, view);
    ASSERT_EQ(r.outcome, PlacementOutcome::PLACED) << r.reason;
    std::set<int> free_set(free.begin(), free.end());
    for (int npu : r.npus) {
        EXPECT_TRUE(free_set.count(npu)) << "NPU " << npu << " not in free set";
    }
}

TEST(L1Clustering, MakeSchedulerReturnsInstance) {
    auto sched = AstraSim::Scheduling::make_placement_policy("l1clustering");
    ASSERT_NE(sched, nullptr);
    EXPECT_EQ(sched->name(), "l1clustering");
}
