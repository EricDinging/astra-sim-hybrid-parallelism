/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/Ljdf.hh"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <vector>

namespace {

using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::DurationEstimator;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::Ljdf;

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

JobInstance make_job(int job_id, AstraSim::Tick arrival) {
    JobArrival a{job_id, arrival, /*num_ranks=*/1, /*shape=*/{1, 1, 1}};
    return JobInstance(a, /*trace_dir=*/"", /*runtime=*/nullptr);
}

ClusterView empty_view() {
    return ClusterView(/*free=*/{}, /*dims=*/{}, /*total=*/0, /*tick=*/0);
}

// Build an Ljdf whose stub maps job_id -> duration, and apply on_arrival to
// each job so est_duration is populated exactly as the runtime would.
Ljdf make_ljdf_and_prime(std::vector<JobInstance*>& pending,
                         std::map<int, AstraSim::Tick> table) {
    Ljdf policy(std::make_unique<StubEstimator>(std::move(table)));
    for (JobInstance* j : pending) {
        policy.on_arrival(*j);
    }
    return policy;
}

TEST(LjdfAdmission, PicksLongestDuration) {
    JobInstance j0 = make_job(0, 0);
    JobInstance j1 = make_job(1, 0);
    JobInstance j2 = make_job(2, 0);
    std::vector<JobInstance*> pending = {&j0, &j1, &j2};
    Ljdf policy = make_ljdf_and_prime(pending, {{0, 300}, {1, 100}, {2, 200}});

    EXPECT_EQ(policy.select_next(pending, empty_view()), &j0);  // 300
}

TEST(LjdfAdmission, OnArrivalCachesEstimate) {
    JobInstance j0 = make_job(0, 0);
    std::vector<JobInstance*> pending = {&j0};
    make_ljdf_and_prime(pending, {{0, 4242}});
    ASSERT_TRUE(j0.est_duration.has_value());
    EXPECT_EQ(j0.est_duration.value(), 4242ULL);
}

TEST(LjdfAdmission, TieOnDurationBreaksByArrivalThenId) {
    JobInstance j0 = make_job(0, 50);
    JobInstance j1 = make_job(1, 30);
    JobInstance j2 = make_job(2, 10);
    std::vector<JobInstance*> pending = {&j0, &j1, &j2};
    Ljdf policy = make_ljdf_and_prime(pending, {{0, 100}, {1, 100}, {2, 100}});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // arrival 10
}

TEST(LjdfAdmission, TieOnDurationAndArrivalBreaksById) {
    JobInstance j5 = make_job(5, 0);
    JobInstance j2 = make_job(2, 0);
    std::vector<JobInstance*> pending = {&j5, &j2};
    Ljdf policy = make_ljdf_and_prime(pending, {{5, 100}, {2, 100}});
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j2);  // lower id
}

TEST(LjdfAdmission, FailedEstimateSortsLast) {
    // A job with no scripted duration gets est_duration = 0 — under
    // largest-first it must lose to any job with a positive estimate
    // (the mirror of Sjdf, where the same zero sorts first).
    JobInstance j0 = make_job(0, 0);
    JobInstance j1 = make_job(1, 10);
    std::vector<JobInstance*> pending = {&j0, &j1};
    Ljdf policy = make_ljdf_and_prime(pending, {{1, 100}});  // j0 -> 0
    EXPECT_EQ(policy.select_next(pending, empty_view()), &j1);
}

TEST(LjdfAdmission, SelectionIndependentOfInsertionOrder) {
    JobInstance j0 = make_job(0, 0);
    JobInstance j1 = make_job(1, 0);
    std::vector<JobInstance*> a = {&j0, &j1};
    std::vector<JobInstance*> b = {&j1, &j0};
    Ljdf pa = make_ljdf_and_prime(a, {{0, 200}, {1, 100}});
    Ljdf pb = make_ljdf_and_prime(b, {{0, 200}, {1, 100}});
    EXPECT_EQ(pa.select_next(a, empty_view()), &j0);
    EXPECT_EQ(pb.select_next(b, empty_view()), &j0);
}

TEST(LjdfAdmission, NullptrOnEmptyPending) {
    std::vector<JobInstance*> pending;
    Ljdf policy = make_ljdf_and_prime(pending, {});
    EXPECT_EQ(policy.select_next(pending, empty_view()), nullptr);
}

}  // namespace
