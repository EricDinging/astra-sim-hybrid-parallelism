/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/AdmissionPolicy.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/Fifo.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

using AstraSim::Scheduling::AdmissionConfig;
using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::Fifo;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::make_admission_policy;

// A JobInstance with the given id and arrival time; trace_dir empty, runtime
// nullptr (both safe for admission tests).
JobInstance make_job(int job_id, AstraSim::Tick arrival) {
    JobArrival a{job_id, arrival, /*num_ranks=*/1, /*shape=*/{1, 1, 1}};
    return JobInstance(a, /*trace_dir=*/"", /*runtime=*/nullptr);
}

ClusterView empty_view() {
    return ClusterView(/*free=*/{}, /*dims=*/{}, /*total=*/0, /*tick=*/0);
}

TEST(FifoAdmission, PicksEarliestArrival) {
    JobInstance j0 = make_job(0, 100);
    JobInstance j1 = make_job(1, 50);
    JobInstance j2 = make_job(2, 75);
    std::vector<JobInstance*> pending = {&j0, &j1, &j2};

    Fifo policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j1);  // arrival 50
}

TEST(FifoAdmission, BreaksArrivalTiesByJobId) {
    JobInstance j5 = make_job(5, 100);
    JobInstance j2 = make_job(2, 100);
    std::vector<JobInstance*> pending = {&j5, &j2};

    Fifo policy;
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // lower id
}

TEST(FifoAdmission, NullptrOnEmptyPending) {
    Fifo policy;
    std::vector<JobInstance*> pending;
    EXPECT_EQ(policy.select_next(pending, empty_view()), nullptr);
}

TEST(FifoAdmission, FactoryBuildsFifo) {
    auto p = make_admission_policy("fifo", AdmissionConfig{});
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "fifo");
}

TEST(FifoAdmission, FactoryNullOnUnknown) {
    EXPECT_EQ(make_admission_policy("nope", AdmissionConfig{}), nullptr);
}

TEST(FifoAdmission, FactoryBuildsLjsf) {
    // ljsf is a pure size policy: no estimator config required.
    auto p = make_admission_policy("ljsf", AdmissionConfig{});
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "ljsf");
    EXPECT_FALSE(p->uses_backfill());
}

TEST(FifoAdmission, FactoryBuildsLjdf) {
    // ljdf needs a duration estimator: supply positive roofline config.
    AdmissionConfig cfg;
    cfg.peak_perf = 1e12;
    cfg.local_mem_bw = 1e9;
    cfg.max_link_bw = 1e9;
    auto p = make_admission_policy("ljdf", cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "ljdf");
    EXPECT_FALSE(p->uses_backfill());
}

TEST(FifoAdmission, FactoryBuildsLwf) {
    // lwf needs a duration estimator: supply positive roofline config.
    AdmissionConfig cfg;
    cfg.peak_perf = 1e12;
    cfg.local_mem_bw = 1e9;
    cfg.max_link_bw = 1e9;
    auto p = make_admission_policy("lwf", cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "lwf");
    EXPECT_FALSE(p->uses_backfill());
}

}  // namespace
