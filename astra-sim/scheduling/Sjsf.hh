/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_SJSF_HH__
#define __ASTRASIM_SCHEDULING_SJSF_HH__

#include "astra-sim/scheduling/AdmissionPolicy.hh"

namespace AstraSim {
namespace Scheduling {

// Smallest-job-size-first admission: smallest (num_ranks, arrival_time,
// job_id) wins — FIFO among size-equals. Pure size policy: needs no duration
// estimator and leaves est_duration unset.
class Sjsf : public AdmissionPolicy {
  public:
    JobInstance* select_next(const std::vector<JobInstance*>& pending,
                             const ClusterView& view) const override;
    std::string name() const override {
        return "sjsf";
    }
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
