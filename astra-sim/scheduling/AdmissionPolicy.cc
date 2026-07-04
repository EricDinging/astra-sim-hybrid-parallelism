/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/AdmissionPolicy.hh"

#include "astra-sim/scheduling/DurationEstimator.hh"
#include "astra-sim/scheduling/Easy.hh"
#include "astra-sim/scheduling/Fifo.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/Ljdf.hh"
#include "astra-sim/scheduling/Ljsf.hh"
#include "astra-sim/scheduling/Ljsfpack.hh"
#include "astra-sim/scheduling/Lwf.hh"
#include "astra-sim/scheduling/Sjdf.hh"
#include "astra-sim/scheduling/Sjsf.hh"
#include "astra-sim/scheduling/Swf.hh"

namespace AstraSim {
namespace Scheduling {

DurationKeyedPolicy::DurationKeyedPolicy(
    std::unique_ptr<DurationEstimator> estimator)
    : estimator_(std::move(estimator)) {}

DurationKeyedPolicy::~DurationKeyedPolicy() = default;

void DurationKeyedPolicy::on_arrival(JobInstance& job) {
    job.est_duration = estimator_->estimate(job);
}

namespace {

// Shared impl of select_min/max_by_key: only the primary comparison direction
// flips. The (arrival_time, job_id) tie-break stays ascending in both: among
// key-equals, the oldest (then lowest id) wins — FIFO fairness.
JobInstance* select_by_key(
    const std::vector<JobInstance*>& pending,
    const std::function<uint64_t(const JobInstance&)>& key,
    bool largest) {
    JobInstance* best = nullptr;
    uint64_t best_key = 0;
    for (JobInstance* job : pending) {
        const uint64_t k = key(*job);
        const bool primary = largest ? k > best_key : k < best_key;
        const bool better =
            best == nullptr || primary ||
            (k == best_key && (job->arrival_time < best->arrival_time ||
                               (job->arrival_time == best->arrival_time &&
                                job->job_id < best->job_id)));
        if (better) {
            best = job;
            best_key = k;
        }
    }
    return best;
}

}  // namespace

JobInstance* select_min_by_key(
    const std::vector<JobInstance*>& pending,
    const std::function<uint64_t(const JobInstance&)>& key) {
    return select_by_key(pending, key, /*largest=*/false);
}

JobInstance* select_max_by_key(
    const std::vector<JobInstance*>& pending,
    const std::function<uint64_t(const JobInstance&)>& key) {
    return select_by_key(pending, key, /*largest=*/true);
}

std::unique_ptr<AdmissionPolicy> make_admission_policy(
    const std::string& name, const AdmissionConfig& cfg) {
    if (name == "fifo") {
        return std::make_unique<Fifo>();
    }
    if (name == "sjsf") {
        return std::make_unique<Sjsf>();  // size-only: no estimator needed
    }
    if (name == "ljsf") {
        return std::make_unique<Ljsf>();  // size-only: no estimator needed
    }
    if (name == "ljsfpack") {
        return std::make_unique<Ljsfpack>();  // size-only: no estimator
    }
    if (name == "sjdf" || name == "ljdf" || name == "swf" || name == "easy" ||
        name == "lwf") {
        auto estimator =
            make_duration_estimator(cfg.estimator_name, cfg.peak_perf,
                                    cfg.local_mem_bw, cfg.max_link_bw);
        if (!estimator) {
            return nullptr;  // unsupported --duration-estimator
        }
        if (name == "sjdf") {
            return std::make_unique<Sjdf>(std::move(estimator));
        }
        if (name == "ljdf") {
            return std::make_unique<Ljdf>(std::move(estimator));
        }
        if (name == "swf") {
            return std::make_unique<Swf>(std::move(estimator));
        }
        if (name == "lwf") {
            return std::make_unique<Lwf>(std::move(estimator));
        }
        return std::make_unique<Easy>(std::move(estimator));
    }
    return nullptr;
}

}  // namespace Scheduling
}  // namespace AstraSim
