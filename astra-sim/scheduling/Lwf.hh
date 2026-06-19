/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_LWF_HH__
#define __ASTRASIM_SCHEDULING_LWF_HH__

#include "astra-sim/scheduling/AdmissionPolicy.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"

#include <memory>

namespace AstraSim {
namespace Scheduling {

// Largest-work-first admission: largest (est_duration * num_ranks,
// arrival_time, job_id) wins (only the product flips to descending). Work =
// estimated NPU-time, the job's space x time "area" — the 2-D FFD-by-area
// mirror of Swf. The estimate is computed once per job in on_arrival, exactly
// like Swf. A failed estimate yields est_duration = 0, zeroing the product, so
// the job sorts LAST under largest-first (the symmetric opposite of Swf, where
// a zeroed product sorts first). The product fits uint64_t comfortably at this
// fork's cluster scales (~1e13 ns * 1e3 ranks ~ 1e16 << 2^64).
class Lwf : public AdmissionPolicy {
  public:
    explicit Lwf(std::unique_ptr<DurationEstimator> estimator);
    void on_arrival(JobInstance& job) override;
    JobInstance* select_next(const std::vector<JobInstance*>& pending,
                             const ClusterView& view) const override;
    std::string name() const override {
        return "lwf";
    }

  private:
    std::unique_ptr<DurationEstimator> estimator_;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
