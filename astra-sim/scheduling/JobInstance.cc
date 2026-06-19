/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/JobInstance.hh"

#include "astra-sim/scheduling/SchedRuntime.hh"
#include "astra-sim/workload/Workload.hh"

#include <cassert>

namespace AstraSim {
namespace Scheduling {

JobInstance::JobInstance(const JobArrival& a, std::string td, SchedRuntime* rt)
    : job_id(a.job_id),
      num_ranks(a.num_ranks),
      shape(a.shape),
      trace_dir(std::move(td)),
      status(JobStatus::PENDING),
      arrival_time(a.arrival_time),
      finished_rank_count(0),
      runtime(rt) {}

JobInstance::~JobInstance() = default;

void JobInstance::on_rank_finished(int /*local_rank*/, Tick t) {
    finished_rank_count++;
    if (finished_rank_count == num_ranks) {
        completed_time = t;
        if (runtime) {
            runtime->post_detach(this);
        }
    }
}

Tick JobInstance::queue_wait() const {
    assert(execution_time.has_value());
    return execution_time.value() - arrival_time;
}

Tick JobInstance::jct() const {
    assert(completed_time.has_value());
    return completed_time.value() - arrival_time;
}

}  // namespace Scheduling
}  // namespace AstraSim
