/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/Ljdf.hh"

#include "astra-sim/scheduling/JobInstance.hh"

namespace AstraSim {
namespace Scheduling {

Ljdf::Ljdf(std::unique_ptr<DurationEstimator> estimator)
    : estimator_(std::move(estimator)) {}

void Ljdf::on_arrival(JobInstance& job) {
    job.est_duration = estimator_->estimate(job);
}

JobInstance* Ljdf::select_next(const std::vector<JobInstance*>& pending,
                               const ClusterView& /*view*/) const {
    return select_max_by_key(pending, [](const JobInstance& job) {
        return static_cast<uint64_t>(job.est_duration.value_or(0));
    });
}

}  // namespace Scheduling
}  // namespace AstraSim
