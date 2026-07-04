/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_LJDF_HH__
#define __ASTRASIM_SCHEDULING_LJDF_HH__

#include "astra-sim/scheduling/AdmissionPolicy.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"

#include <memory>

namespace AstraSim {
namespace Scheduling {

// Longest-job-duration-first admission: largest est_duration wins, with the
// (arrival_time, job_id) tie-break ascending — the duration-axis mirror of
// Sjdf. The estimate is computed once per job in on_arrival, exactly like
// Sjdf. A failed estimate yields est_duration = 0, sorting the job last (in
// Sjdf the same zero sorts it first); this matches the Lwf convention.
class Ljdf : public DurationKeyedPolicy {
  public:
    using DurationKeyedPolicy::DurationKeyedPolicy;
    JobInstance* select_next(const std::vector<JobInstance*>& pending,
                             const ClusterView& view) const override;
    std::string name() const override {
        return "ljdf";
    }
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
