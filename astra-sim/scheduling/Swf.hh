/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_SWF_HH__
#define __ASTRASIM_SCHEDULING_SWF_HH__

#include "astra-sim/scheduling/AdmissionPolicy.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"

#include <memory>

namespace AstraSim {
namespace Scheduling {

// Smallest-work-first admission: smallest (est_duration * num_ranks,
// arrival_time, job_id) wins. Work = estimated NPU-time; the product fits
// uint64_t comfortably at this fork's cluster scales (~1e13 ns * 1e3 ranks
// ~ 1e16 << 2^64; num_ranks carries no enforced upper bound). The duration
// estimate is computed once per job in on_arrival, exactly like Sjdf. A
// failed estimate yields est_duration = 0, zeroing the product, so the job
// admits first regardless of its size — same fallback as Sjdf, but here
// even a huge rank count cannot temper it.
class Swf : public AdmissionPolicy {
  public:
    explicit Swf(std::unique_ptr<DurationEstimator> estimator);
    void on_arrival(JobInstance& job) override;
    JobInstance* select_next(const std::vector<JobInstance*>& pending,
                             const ClusterView& view) const override;
    std::string name() const override {
        return "swf";
    }

  private:
    std::unique_ptr<DurationEstimator> estimator_;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
