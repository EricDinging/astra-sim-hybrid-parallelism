/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/Ljsf.hh"

#include <gtest/gtest.h>

#include <vector>

namespace {

using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::Ljsf;

JobInstance make_job(int job_id, AstraSim::Tick arrival, int num_ranks) {
    JobArrival a{job_id, arrival, num_ranks, /*shape=*/{num_ranks, 1, 1}};
    return JobInstance(a, /*trace_dir=*/"", /*runtime=*/nullptr);
}

ClusterView empty_view() {
    return ClusterView(/*free=*/{}, /*dims=*/{}, /*total=*/0, /*tick=*/0);
}

TEST(LjsfAdmission, NameIsLjsf) {
    Ljsf policy;
    EXPECT_EQ(policy.name(), "ljsf");
}

TEST(LjsfAdmission, UsesBackfillIsFalse) {
    Ljsf policy;
    EXPECT_FALSE(policy.uses_backfill());
}

TEST(LjsfAdmission, PicksLargestSize) {
    JobInstance j0 = make_job(0, 0, 8);
    JobInstance j1 = make_job(1, 0, 2);
    JobInstance j2 = make_job(2, 0, 16);
    std::vector<JobInstance*> pending = {&j0, &j1, &j2};
    Ljsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // 16 ranks
}

TEST(LjsfAdmission, SizeBeatsArrivalOrder) {
    // Smaller job arrived first; largest size must win over arrival.
    JobInstance early_small = make_job(0, 10, 4);
    JobInstance late_big = make_job(1, 50, 16);
    std::vector<JobInstance*> pending = {&early_small, &late_big};
    Ljsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &late_big);
}

TEST(LjsfAdmission, IgnoresEstDuration) {
    // A large est_duration on the small job must not beat a larger size.
    JobInstance big = make_job(0, 0, 64);
    JobInstance small = make_job(1, 0, 4);
    big.est_duration = 1;
    small.est_duration = 1000000;
    std::vector<JobInstance*> pending = {&big, &small};
    Ljsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &big);
}

TEST(LjsfAdmission, TieOnSizeBreaksByArrivalThenId) {
    JobInstance j0 = make_job(0, 50, 8);
    JobInstance j1 = make_job(1, 30, 8);
    JobInstance j2 = make_job(2, 10, 8);
    std::vector<JobInstance*> pending = {&j0, &j1, &j2};
    Ljsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // arrival 10
}

TEST(LjsfAdmission, TieOnSizeAndArrivalBreaksById) {
    JobInstance j5 = make_job(5, 0, 8);
    JobInstance j2 = make_job(2, 0, 8);
    std::vector<JobInstance*> pending = {&j5, &j2};
    Ljsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // lower id
}

TEST(LjsfAdmission, SelectionIndependentOfInsertionOrder) {
    JobInstance j0 = make_job(0, 0, 16);
    JobInstance j1 = make_job(1, 0, 4);
    std::vector<JobInstance*> a = {&j0, &j1};
    std::vector<JobInstance*> b = {&j1, &j0};
    Ljsf policy;
    EXPECT_EQ(policy.select_next(a, empty_view()), &j0);
    EXPECT_EQ(policy.select_next(b, empty_view()), &j0);
}

TEST(LjsfAdmission, OnArrivalLeavesEstDurationUnset) {
    JobInstance j0 = make_job(0, 0, 4);
    Ljsf policy;
    policy.on_arrival(j0);  // base no-op; ljsf needs no durations
    EXPECT_FALSE(j0.est_duration.has_value());
}

TEST(LjsfAdmission, NullptrOnEmptyPending) {
    std::vector<JobInstance*> pending;
    Ljsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), nullptr);
}

}  // namespace
