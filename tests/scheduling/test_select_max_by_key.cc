/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/AdmissionPolicy.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::select_max_by_key;

JobInstance make_job(int job_id, AstraSim::Tick arrival, int num_ranks) {
    JobArrival a{job_id, arrival, num_ranks, /*shape=*/{num_ranks, 1, 1}};
    return JobInstance(a, /*trace_dir=*/"", /*runtime=*/nullptr);
}

uint64_t by_ranks(const JobInstance& job) {
    return static_cast<uint64_t>(job.num_ranks);
}

TEST(SelectMaxByKey, PicksLargestKey) {
    JobInstance j0 = make_job(0, 0, 8);
    JobInstance j1 = make_job(1, 0, 32);
    JobInstance j2 = make_job(2, 0, 4);
    std::vector<JobInstance*> pending = {&j0, &j1, &j2};
    EXPECT_EQ(select_max_by_key(pending, by_ranks), &j1);  // 32 ranks
}

TEST(SelectMaxByKey, TieBreaksByEarliestArrival) {
    JobInstance late = make_job(0, 50, 16);
    JobInstance early = make_job(1, 10, 16);  // equal key, earlier arrival
    std::vector<JobInstance*> pending = {&late, &early};
    EXPECT_EQ(select_max_by_key(pending, by_ranks), &early);
}

TEST(SelectMaxByKey, TieBreaksByLowestIdWhenArrivalEqual) {
    JobInstance j5 = make_job(5, 0, 16);
    JobInstance j2 = make_job(2, 0, 16);  // equal key+arrival, lower id
    std::vector<JobInstance*> pending = {&j5, &j2};
    EXPECT_EQ(select_max_by_key(pending, by_ranks), &j2);
}

TEST(SelectMaxByKey, SingleElement) {
    JobInstance j0 = make_job(0, 0, 4);
    std::vector<JobInstance*> pending = {&j0};
    EXPECT_EQ(select_max_by_key(pending, by_ranks), &j0);
}

TEST(SelectMaxByKey, NullptrOnEmpty) {
    std::vector<JobInstance*> pending;
    EXPECT_EQ(select_max_by_key(pending, by_ranks), nullptr);
}

}  // namespace
