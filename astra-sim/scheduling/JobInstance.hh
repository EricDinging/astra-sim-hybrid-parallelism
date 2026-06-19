/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_JOBINSTANCE_HH__
#define __ASTRASIM_SCHEDULING_JOBINSTANCE_HH__

#include "astra-sim/scheduling/Common.hh"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AstraSim {
class Workload;
}

namespace AstraSim {
namespace Scheduling {

class SchedRuntime;

class JobInstance {
  public:
    JobInstance(const JobArrival& arrival,
                std::string trace_dir,
                SchedRuntime* runtime);
    ~JobInstance();

    // Identity / shape.
    int job_id;
    int num_ranks;
    std::array<int, 3> shape;
    std::string trace_dir;
    JobStatus status;

    // Lifecycle timestamps (ns).
    Tick arrival_time;
    std::optional<Tick> execution_time;  // set on PLACED
    std::optional<Tick> completed_time;  // set when the last rank finishes

    // Estimated duration cached in on_arrival by policies that need durations
    // (sjdf, ljdf, swf, easy, lwf); empty under policies that never read
    // durations (fifo, sjsf, ljsf).
    std::optional<Tick> est_duration;

    // Set from PlacementResult::ordered_rings at placement (Decision D4).
    bool ordered_rings = false;

    // Set at placement time; size==K, idx==job_local_rank, value==global NPU
    // id.
    std::vector<int> rank_map;

    // K Workloads, owned. Created at place_job, destroyed when JobRegistry is.
    std::vector<std::unique_ptr<Workload>> rank_workloads;

    // Bookkeeping for "all K ranks finished".
    int finished_rank_count;

    // Non-owning back-pointer. Used to post detach into the event queue.
    SchedRuntime* runtime;

    // Called by Workload's finish predicate after is_finished flips.
    void on_rank_finished(int local_rank, Tick t);

    // Convenience: queue_wait = execution_time - arrival_time
    //              jct        = completed_time - arrival_time (end-to-end)
    Tick queue_wait() const;
    Tick jct() const;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
