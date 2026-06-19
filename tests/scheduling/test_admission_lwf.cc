/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/Lwf.hh"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <vector>

namespace {

using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::DurationEstimator;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::Lwf;

// Stub estimator: scripted duration per job_id, no trace I/O.
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

Lwf make_lwf_and_prime(std::vector<JobInstance*>& pending,
                       std::map<int, AstraSim::Tick> table) {
    Lwf policy(std::make_unique<StubEstimator>(std::move(table)));
    for (JobInstance* j : pending) {
        policy.on_arrival(*j);
    }
    return policy;
}

TEST(LwfAdmission, NameIsLwf) {
    std::vector<JobInstance*> pending;
    Lwf policy = make_lwf_and_prime(pending, {});
    EXPECT_EQ(policy.name(), "lwf");
}

TEST(LwfAdmission, UsesBackfillIsFalse) {
    std::vector<JobInstance*> pending;
    Lwf policy = make_lwf_and_prime(pending, {});
    EXPECT_FALSE(policy.uses_backfill());
}

TEST(LwfAdmission, OnArrivalCachesEstimate) {
    JobInstance j0 = make_job(0, 0, 4);
    std::vector<JobInstance*> pending = {&j0};
    make_lwf_and_prime(pending, {{0, 4242}});
    ASSERT_TRUE(j0.est_duration.has_value());
    EXPECT_EQ(j0.est_duration.value(), 4242ULL);
}

TEST(LwfAdmission, PicksLargestWorkNotLargestDuration) {
    // long-narrow vs short-wide: duration order and work order disagree.
    JobInstance narrow = make_job(0, 0, 8);  // 300 * 8  = 2400
    JobInstance wide = make_job(1, 0, 64);   // 100 * 64 = 6400
    std::vector<JobInstance*> pending = {&narrow, &wide};
    Lwf policy = make_lwf_and_prime(pending, {{0, 300}, {1, 100}});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &wide);  // work 6400
}

TEST(LwfAdmission, PicksLargestWorkNotLargestSize) {
    // few-rank long job beats many-rank short job on the product.
    JobInstance many_short = make_job(0, 0, 64);  // 10 * 64  = 640
    JobInstance few_long = make_job(1, 0, 8);     // 1000 * 8 = 8000
    std::vector<JobInstance*> pending = {&many_short, &few_long};
    Lwf policy = make_lwf_and_prime(pending, {{0, 10}, {1, 1000}});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &few_long);
}

TEST(LwfAdmission, FailedEstimateSortsLast) {
    // No table entry -> estimate 0 -> work 0 -> sorts LAST under largest-first
    // (the symmetric opposite of swf, where a zeroed product sorts first).
    JobInstance estimated = make_job(0, 0, 4);       // 100 * 4 = 400
    JobInstance unestimated = make_job(1, 0, 1000);  // work 0
    std::vector<JobInstance*> pending = {&estimated, &unestimated};
    Lwf policy = make_lwf_and_prime(pending, {{0, 100}});  // j1 unestimated
    EXPECT_EQ(policy.select_next(pending, empty_view()), &estimated);
}

TEST(LwfAdmission, TieOnWorkBreaksByArrivalThenId) {
    JobInstance j0 = make_job(0, 50, 4);  // 100 * 4 = 400
    JobInstance j1 = make_job(1, 10, 8);  // 50 * 8  = 400
    std::vector<JobInstance*> pending = {&j0, &j1};
    Lwf policy = make_lwf_and_prime(pending, {{0, 100}, {1, 50}});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j1);  // arrival 10
}

TEST(LwfAdmission, TieOnWorkAndArrivalBreaksById) {
    JobInstance j5 = make_job(5, 0, 4);
    JobInstance j2 = make_job(2, 0, 4);
    std::vector<JobInstance*> pending = {&j5, &j2};
    Lwf policy = make_lwf_and_prime(pending, {{5, 100}, {2, 100}});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // lower id
}

TEST(LwfAdmission, SelectionIndependentOfInsertionOrder) {
    JobInstance j0 = make_job(0, 0, 8);  // 100 * 8 = 800
    JobInstance j1 = make_job(1, 0, 4);  // 50 * 4  = 200
    std::vector<JobInstance*> a = {&j0, &j1};
    std::vector<JobInstance*> b = {&j1, &j0};
    Lwf pa = make_lwf_and_prime(a, {{0, 100}, {1, 50}});
    Lwf pb = make_lwf_and_prime(b, {{0, 100}, {1, 50}});
    EXPECT_EQ(pa.select_next(a, empty_view()), &j0);
    EXPECT_EQ(pb.select_next(b, empty_view()), &j0);
}

TEST(LwfAdmission, NullptrOnEmptyPending) {
    std::vector<JobInstance*> pending;
    Lwf policy = make_lwf_and_prime(pending, {});
    EXPECT_EQ(policy.select_next(pending, empty_view()), nullptr);
}

}  // namespace
