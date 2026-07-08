/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_JOBSTATSWRITER_HH__
#define __ASTRASIM_SCHEDULING_JOBSTATSWRITER_HH__

#include "astra-sim/common/Common.hh"

#include <fstream>
#include <string>
#include <vector>

namespace AstraSim {
namespace Scheduling {

class JobInstance;
class JobRegistry;

class JobStatsWriter {
  public:
    explicit JobStatsWriter(std::string output_dir);

    // Writes <output_dir>/jobs.csv, one row per JobInstance in job_id order.
    void emit_jobs_csv(const JobRegistry& registry);

    // Writes <output_dir>/summary.txt with aggregate stats.
    void emit_summary(const JobRegistry& registry,
                      const std::string& admission_name,
                      const std::string& placement_name,
                      int total_npus);

    // Writes <output_dir>/node_jobs.csv: one row per NPU listing the job ids
    // that ran on it over the simulation. Idle NPUs get a row with no jobs.
    void emit_node_jobs(const JobRegistry& registry, int total_npus);

    // Writes <output_dir>/failed_nodes.log: a header line "failed_npu_id"
    // followed by one NPU id per line in the order supplied by the caller
    // (callers pass ids ascending). Written even when empty (header only) so
    // every run directory has a uniform artifact set.
    void emit_failed_nodes(const std::vector<int>& failed_npus);

    // Appends one row per completed job to <output_dir>/jct.csv and flushes,
    // so per-job JCTs are readable while the simulation is still running.
    // Rows are in completion order (not job_id order).
    void stream_jct_row(const JobInstance& job);

    // Appends the post-change cluster occupancy to <output_dir>/occupancy.csv
    // and flushes: one row per placement/completion event. Several events can
    // share a tick; the last row at a tick is the settled occupancy.
    void stream_occupancy_row(Tick tick, int busy_npus, int total_npus);

  private:
    std::string output_dir_;
    std::ofstream jct_out_;  // lazily opened by stream_jct_row
    std::ofstream occ_out_;  // lazily opened by stream_occupancy_row
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
