/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_SJDF_HH__
#define __ASTRASIM_SCHEDULING_SJDF_HH__

#include "astra-sim/scheduling/AdmissionPolicy.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"

#include <memory>

namespace AstraSim {
namespace Scheduling {

// Shortest-job-duration-first admission: smallest (est_duration, arrival_time,
// job_id) wins. The duration estimate is computed once per job in on_arrival
// via an injected DurationEstimator.
class Sjdf : public AdmissionPolicy {
  public:
    explicit Sjdf(std::unique_ptr<DurationEstimator> estimator);
    void on_arrival(JobInstance& job) override;
    JobInstance* select_next(const std::vector<JobInstance*>& pending,
                             const ClusterView& view) const override;
    std::string name() const override {
        return "sjdf";
    }

  private:
    std::unique_ptr<DurationEstimator> estimator_;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
