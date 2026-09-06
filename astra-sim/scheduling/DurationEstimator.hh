/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_DURATIONESTIMATOR_HH__
#define __ASTRASIM_SCHEDULING_DURATIONESTIMATOR_HH__

#include "astra-sim/common/Common.hh"

#include <memory>
#include <string>
#include <unordered_map>

namespace AstraSim {
namespace Scheduling {

class JobInstance;

// Strategy that estimates a job's running time (ns) before it runs.
class DurationEstimator {
  public:
    virtual ~DurationEstimator() = default;
    virtual Tick estimate(const JobInstance& job) const = 0;
    virtual std::string name() const = 0;
};

// Default estimator: rank-0 roofline compute term + comm term.
//   compute: sum over COMP nodes of num_ops / min(local_mem_bw*oi, peak_perf)
//   comm:    sum over COMM nodes of comm_size / max_link_bw
// Rates are SI: peak_perf in FLOP/s, the two bandwidths in bytes/s.
class RooflineCommEstimator : public DurationEstimator {
  public:
    RooflineCommEstimator(double peak_perf,
                          double local_mem_bw,
                          double max_link_bw);
    Tick estimate(const JobInstance& job) const override;
    std::string name() const override {
        return "roofline-comm";
    }

  private:
    double peak_perf_;
    double local_mem_bw_;
    double max_link_bw_;
};

// Measured-table estimator: est = svc_per_iter_ns[shape] * num_iterations,
// from a CSV with a header row containing `shape` and `svc_per_iter_ns`
// columns (examples/t3d/service_times.csv: each shape's isolated
// single-iteration JCT). On the t3d pareto traces it predicts the realized
// duration to within 10% at p90, where roofline-comm underestimates by 2.4x
// median (it prices one iteration, and a lower bound at that) -- so with
// roofline-comm every running job looks overdue and EASY's shadow time
// collapses to "now". Exits on a shape missing from the table.
class SvcTableEstimator : public DurationEstimator {
  public:
    explicit SvcTableEstimator(const std::string& csv_path);
    Tick estimate(const JobInstance& job) const override;
    std::string name() const override {
        return "svc-table";
    }

  private:
    std::unordered_map<std::string, Tick> svc_per_iter_;
};

// Returns nullptr on an unknown estimator name. `svc_table_path` is read only
// by svc-table; the three rates only by roofline-comm.
std::unique_ptr<DurationEstimator> make_duration_estimator(
    const std::string& name,
    double peak_perf,
    double local_mem_bw,
    double max_link_bw,
    const std::string& svc_table_path = "");

}  // namespace Scheduling
}  // namespace AstraSim

#endif
