/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/Sjsf.hh"

#include <gtest/gtest.h>

#include <vector>

namespace {

using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::Sjsf;

JobInstance make_job(int job_id, AstraSim::Tick arrival, int num_ranks) {
    JobArrival a{job_id, arrival, num_ranks, /*shape=*/{num_ranks, 1, 1}};
    return JobInstance(a, /*trace_dir=*/"", /*runtime=*/nullptr);
}

ClusterView empty_view() {
    return ClusterView(/*free=*/{}, /*dims=*/{}, /*total=*/0, /*tick=*/0);
}

TEST(SjsfAdmission, PicksSmallestSize) {
    JobInstance j0 = make_job(0, 0, 8);
    JobInstance j1 = make_job(1, 0, 2);
    JobInstance j2 = make_job(2, 0, 4);
    std::vector<JobInstance*> pending = {&j0, &j1, &j2};
    Sjsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j1);  // 2 ranks
}

TEST(SjsfAdmission, SizeBeatsArrivalOrder) {
    // Larger job arrived first; size must win over arrival (unlike fifo).
    JobInstance early_big = make_job(0, 10, 16);
    JobInstance late_small = make_job(1, 50, 4);
    std::vector<JobInstance*> pending = {&early_big, &late_small};
    Sjsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &late_small);
}

TEST(SjsfAdmission, IgnoresEstDuration) {
    // A tiny est_duration on the large job must not beat a smaller size.
    JobInstance big = make_job(0, 0, 64);
    JobInstance small = make_job(1, 0, 4);
    big.est_duration = 1;
    small.est_duration = 1000000;
    std::vector<JobInstance*> pending = {&big, &small};
    Sjsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &small);
}

TEST(SjsfAdmission, TieOnSizeBreaksByArrivalThenId) {
    JobInstance j0 = make_job(0, 50, 8);
    JobInstance j1 = make_job(1, 30, 8);
    JobInstance j2 = make_job(2, 10, 8);
    std::vector<JobInstance*> pending = {&j0, &j1, &j2};
    Sjsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // arrival 10
}

TEST(SjsfAdmission, TieOnSizeAndArrivalBreaksById) {
    JobInstance j5 = make_job(5, 0, 8);
    JobInstance j2 = make_job(2, 0, 8);
    std::vector<JobInstance*> pending = {&j5, &j2};
    Sjsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // lower id
}

TEST(SjsfAdmission, SelectionIndependentOfInsertionOrder) {
    JobInstance j0 = make_job(0, 0, 16);
    JobInstance j1 = make_job(1, 0, 4);
    std::vector<JobInstance*> a = {&j0, &j1};
    std::vector<JobInstance*> b = {&j1, &j0};
    Sjsf policy;
    EXPECT_EQ(policy.select_next(a, empty_view()), &j1);
    EXPECT_EQ(policy.select_next(b, empty_view()), &j1);
}

TEST(SjsfAdmission, NullptrOnEmptyPending) {
    std::vector<JobInstance*> pending;
    Sjsf policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), nullptr);
}

}  // namespace
