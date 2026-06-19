/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/Ljsfpack.hh"

#include <gtest/gtest.h>

#include <vector>

namespace {

using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::Ljsfpack;

JobInstance make_job(int job_id, AstraSim::Tick arrival, int num_ranks) {
    JobArrival a{job_id, arrival, num_ranks, /*shape=*/{num_ranks, 1, 1}};
    return JobInstance(a, /*trace_dir=*/"", /*runtime=*/nullptr);
}

ClusterView empty_view() {
    return ClusterView(/*free=*/{}, /*dims=*/{}, /*total=*/0, /*tick=*/0);
}

TEST(LjsfpackAdmission, NameIsLjsfpack) {
    Ljsfpack policy;
    EXPECT_EQ(policy.name(), "ljsfpack");
}

TEST(LjsfpackAdmission, UsesBackfillIsFalse) {
    Ljsfpack policy;
    EXPECT_FALSE(policy.uses_backfill());
}

// The defining difference from Ljsf: a DEFERed job is skipped, not blocked.
// SchedRuntime::sweep reads this flag to dispatch to greedy_sweep().
TEST(LjsfpackAdmission, SkipsOnDeferIsTrue) {
    Ljsfpack policy;
    EXPECT_TRUE(policy.skips_on_defer());
}

// Ordering is inherited verbatim from Ljsf: largest size first.
TEST(LjsfpackAdmission, PicksLargestSize) {
    JobInstance j0 = make_job(0, 0, 8);
    JobInstance j1 = make_job(1, 0, 2);
    JobInstance j2 = make_job(2, 0, 16);
    std::vector<JobInstance*> pending = {&j0, &j1, &j2};
    Ljsfpack policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // 16 ranks
}

TEST(LjsfpackAdmission, SizeBeatsArrivalOrder) {
    JobInstance early_small = make_job(0, 10, 4);
    JobInstance late_big = make_job(1, 50, 16);
    std::vector<JobInstance*> pending = {&early_small, &late_big};
    Ljsfpack policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &late_big);
}

TEST(LjsfpackAdmission, TieOnSizeBreaksByArrivalThenId) {
    JobInstance j0 = make_job(0, 50, 8);
    JobInstance j1 = make_job(1, 30, 8);
    JobInstance j2 = make_job(2, 10, 8);
    std::vector<JobInstance*> pending = {&j0, &j1, &j2};
    Ljsfpack policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // arrival 10
}

TEST(LjsfpackAdmission, NullptrOnEmptyPending) {
    std::vector<JobInstance*> pending;
    Ljsfpack policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), nullptr);
}

}  // namespace
