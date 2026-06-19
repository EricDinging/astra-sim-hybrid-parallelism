/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_DURATIONESTIMATOR_HH__
#define __ASTRASIM_SCHEDULING_DURATIONESTIMATOR_HH__

#include "astra-sim/common/Common.hh"

#include <memory>
#include <string>

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

// Returns nullptr on an unknown estimator name.
std::unique_ptr<DurationEstimator> make_duration_estimator(
    const std::string& name,
    double peak_perf,
    double local_mem_bw,
    double max_link_bw);

}  // namespace Scheduling
}  // namespace AstraSim

#endif
