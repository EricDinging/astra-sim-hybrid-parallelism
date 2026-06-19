/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/Easy.hh"

#include "astra-sim/scheduling/JobInstance.hh"

namespace AstraSim {
namespace Scheduling {

Easy::Easy(std::unique_ptr<DurationEstimator> estimator)
    : estimator_(std::move(estimator)) {}

void Easy::on_arrival(JobInstance& job) {
    job.est_duration = estimator_->estimate(job);
}

JobInstance* Easy::select_next(const std::vector<JobInstance*>& pending,
                               const ClusterView& /*view*/) const {
    // FCFS: smallest (arrival_time, job_id). select_min_by_key tie-breaks by
    // (arrival_time, job_id) already, so keying on arrival_time yields exactly
    // FCFS order.
    return select_min_by_key(pending, [](const JobInstance& job) {
        return static_cast<uint64_t>(job.arrival_time);
    });
}

}  // namespace Scheduling
}  // namespace AstraSim
