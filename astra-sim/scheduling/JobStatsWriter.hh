/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_JOBSTATSWRITER_HH__
#define __ASTRASIM_SCHEDULING_JOBSTATSWRITER_HH__

#include <string>
#include <vector>

namespace AstraSim {
namespace Scheduling {

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

  private:
    std::string output_dir_;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
