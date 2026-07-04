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
    // FCFS: select_min_by_key already tie-breaks by (arrival_time, job_id),
    // so keying on arrival_time yields exactly FIFO order.
    return select_min_by_key(pending, [](const JobInstance& job) {
        return static_cast<uint64_t>(job.arrival_time);
    });
}

}  // namespace Scheduling
}  // namespace AstraSim
