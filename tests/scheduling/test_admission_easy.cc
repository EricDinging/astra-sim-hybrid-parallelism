/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"
#include "astra-sim/scheduling/Easy.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <vector>

namespace {

using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::DurationEstimator;
using AstraSim::Scheduling::Easy;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;

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

Easy make_easy_and_prime(std::vector<JobInstance*>& pending,
                         std::map<int, AstraSim::Tick> table) {
    Easy policy(std::make_unique<StubEstimator>(std::move(table)));
    for (JobInstance* j : pending) {
        policy.on_arrival(*j);
    }
    return policy;
}

TEST(EasyAdmission, NameIsEasy) {
    std::vector<JobInstance*> pending;
    Easy policy = make_easy_and_prime(pending, {});
    EXPECT_EQ(policy.name(), "easy");
}

TEST(EasyAdmission, UsesBackfillIsTrue) {
    std::vector<JobInstance*> pending;
    Easy policy = make_easy_and_prime(pending, {});
    EXPECT_TRUE(policy.uses_backfill());
}

TEST(EasyAdmission, OnArrivalCachesEstimate) {
    JobInstance j0 = make_job(0, 0, 4);
    std::vector<JobInstance*> pending = {&j0};
    make_easy_and_prime(pending, {{0, 4242}});
    ASSERT_TRUE(j0.est_duration.has_value());
    EXPECT_EQ(j0.est_duration.value(), 4242ULL);
}

TEST(EasyAdmission, SelectNextReturnsFcfsHead) {
    JobInstance late = make_job(0, 500, 4);
    JobInstance early = make_job(1, 100, 64);  // larger, but earlier arrival
    std::vector<JobInstance*> pending = {&late, &early};
    Easy policy = make_easy_and_prime(pending, {{0, 10}, {1, 9999}});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &early);
}

TEST(EasyAdmission, SelectNextTieBreaksById) {
    JobInstance j5 = make_job(5, 0, 4);
    JobInstance j2 = make_job(2, 0, 4);
    std::vector<JobInstance*> pending = {&j5, &j2};
    Easy policy = make_easy_and_prime(pending, {});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // lower id
}

TEST(EasyAdmission, SelectionIndependentOfInsertionOrder) {
    JobInstance j0 = make_job(0, 200, 4);
    JobInstance j1 = make_job(1, 100, 4);  // earlier arrival
    std::vector<JobInstance*> a = {&j0, &j1};
    std::vector<JobInstance*> b = {&j1, &j0};
    Easy pa = make_easy_and_prime(a, {});
    Easy pb = make_easy_and_prime(b, {});
    EXPECT_EQ(pa.select_next(a, empty_view()), &j1);
    EXPECT_EQ(pb.select_next(b, empty_view()), &j1);
}

TEST(EasyAdmission, NullptrOnEmptyPending) {
    std::vector<JobInstance*> pending;
    Easy policy = make_easy_and_prime(pending, {});
    EXPECT_EQ(policy.select_next(pending, empty_view()), nullptr);
}

}  // namespace
