/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/Swf.hh"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <vector>

namespace {

using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::DurationEstimator;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::Swf;

// Stub estimator: returns a scripted duration per job_id, no trace I/O.
class StubEstimator : public DurationEstimator {
  public:
    explicit StubEstimator(std::map<int, AstraSim::Tick> table)
        : table_(std::move(table)) {}
    AstraSim::Tick estimate(const JobInstance& job) const override {
        auto it = table_.find(job.job_id);
        return it == table_.end() ? 0 : it->second;
    }
    std::string name() const override {
        return "stub";
    }

  private:
    std::map<int, AstraSim::Tick> table_;
};

JobInstance make_job(int job_id, AstraSim::Tick arrival, int num_ranks) {
    JobArrival a{job_id, arrival, num_ranks, /*shape=*/{num_ranks, 1, 1}};
    return JobInstance(a, /*trace_dir=*/"", /*runtime=*/nullptr);
}

ClusterView empty_view() {
    return ClusterView(/*free=*/{}, /*dims=*/{}, /*total=*/0, /*tick=*/0);
}

Swf make_swf_and_prime(std::vector<JobInstance*>& pending,
                       std::map<int, AstraSim::Tick> table) {
    Swf policy(std::make_unique<StubEstimator>(std::move(table)));
    for (JobInstance* j : pending) {
        policy.on_arrival(*j);
    }
    return policy;
}

TEST(SwfAdmission, PicksSmallestWorkNotSmallestDuration) {
    // Short-wide vs long-narrow: duration order and work order disagree.
    JobInstance wide = make_job(0, 0, 64);   // 100 * 64 = 6400
    JobInstance narrow = make_job(1, 0, 8);  // 300 * 8  = 2400
    std::vector<JobInstance*> pending = {&wide, &narrow};
    Swf policy = make_swf_and_prime(pending, {{0, 100}, {1, 300}});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &narrow);
}

TEST(SwfAdmission, OnArrivalCachesEstimate) {
    JobInstance j0 = make_job(0, 0, 4);
    std::vector<JobInstance*> pending = {&j0};
    make_swf_and_prime(pending, {{0, 4242}});
    ASSERT_TRUE(j0.est_duration.has_value());
    EXPECT_EQ(j0.est_duration.value(), 4242ULL);
}

TEST(SwfAdmission, FailedEstimateSortsFirst) {
    // No table entry -> estimate 0 (a populated zero, not an unset optional)
    // -> work 0 -> sorts first (same admit-first fallback as sjdf).
    JobInstance j0 = make_job(0, 0, 4);
    JobInstance j1 = make_job(1, 0, 4);
    std::vector<JobInstance*> pending = {&j0, &j1};
    Swf policy = make_swf_and_prime(pending, {{0, 100}});  // j1 unestimated
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j1);
}

TEST(SwfAdmission, FailedEstimateBeatsEstimatedSmallJob) {
    // A failed estimate zeroes the product, so even a huge job sorts ahead
    // of a correctly estimated tiny one (documented in Swf.hh).
    JobInstance wide = make_job(0, 0, 1000);  // no table entry -> work 0
    JobInstance small = make_job(1, 0, 8);    // 5 * 8 = 40
    std::vector<JobInstance*> pending = {&wide, &small};
    Swf policy = make_swf_and_prime(pending, {{1, 5}});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &wide);
}

TEST(SwfAdmission, SelectionIndependentOfInsertionOrder) {
    JobInstance j0 = make_job(0, 0, 8);  // 100 * 8 = 800
    JobInstance j1 = make_job(1, 0, 4);  // 50 * 4  = 200
    std::vector<JobInstance*> a = {&j0, &j1};
    std::vector<JobInstance*> b = {&j1, &j0};
    Swf pa = make_swf_and_prime(a, {{0, 100}, {1, 50}});
    Swf pb = make_swf_and_prime(b, {{0, 100}, {1, 50}});
    EXPECT_EQ(pa.select_next(a, empty_view()), &j1);
    EXPECT_EQ(pb.select_next(b, empty_view()), &j1);
}

TEST(SwfAdmission, TieOnWorkBreaksByArrivalThenId) {
    JobInstance j0 = make_job(0, 50, 4);  // 100 * 4 = 400
    JobInstance j1 = make_job(1, 10, 8);  // 50 * 8  = 400
    std::vector<JobInstance*> pending = {&j0, &j1};
    Swf policy = make_swf_and_prime(pending, {{0, 100}, {1, 50}});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j1);  // arrival 10
}

TEST(SwfAdmission, TieOnWorkAndArrivalBreaksById) {
    JobInstance j5 = make_job(5, 0, 4);
    JobInstance j2 = make_job(2, 0, 4);
    std::vector<JobInstance*> pending = {&j5, &j2};
    Swf policy = make_swf_and_prime(pending, {{5, 100}, {2, 100}});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // lower id
}

TEST(SwfAdmission, NullptrOnEmptyPending) {
    std::vector<JobInstance*> pending;
    Swf policy = make_swf_and_prime(pending, {});
    EXPECT_EQ(policy.select_next(pending, empty_view()), nullptr);
}

}  // namespace
