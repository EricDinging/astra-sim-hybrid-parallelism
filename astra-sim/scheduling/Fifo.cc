/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/Fifo.hh"

#include "astra-sim/scheduling/JobInstance.hh"

namespace AstraSim {
namespace Scheduling {

JobInstance* Fifo::select_next(const std::vector<JobInstance*>& pending,
                               const ClusterView& /*view*/) const {
    JobInstance* best = nullptr;
    for (JobInstance* job : pending) {
        if (best == nullptr || job->arrival_time < best->arrival_time ||
            (job->arrival_time == best->arrival_time &&
             job->job_id < best->job_id)) {
            best = job;
        }
    }
    return best;
}

}  // namespace Scheduling
}  // namespace AstraSim
