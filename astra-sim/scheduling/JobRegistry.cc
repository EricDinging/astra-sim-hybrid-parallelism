/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/JobRegistry.hh"

namespace AstraSim {
namespace Scheduling {

JobInstance* JobRegistry::create(const JobArrival& arrival,
                                 SchedRuntime* runtime) {
    auto ji = std::make_unique<JobInstance>(arrival,
                                            /*trace_dir=*/"", runtime);
    JobInstance* ptr = ji.get();
    by_id_.emplace(arrival.job_id, std::move(ji));
    return ptr;
}

bool JobRegistry::no_running_jobs() const {
    for (const auto& [id, ji] : by_id_) {
        (void)id;
        if (ji->status == JobStatus::RUNNING) {
            return false;
        }
    }
    return true;
}

}  // namespace Scheduling
}  // namespace AstraSim
