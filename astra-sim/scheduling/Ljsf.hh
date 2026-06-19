/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_LJSF_HH__
#define __ASTRASIM_SCHEDULING_LJSF_HH__

#include "astra-sim/scheduling/AdmissionPolicy.hh"

namespace AstraSim {
namespace Scheduling {

// Largest-job-size-first admission: largest (num_ranks, arrival_time, job_id)
// wins — FIFO among size-equals (only num_ranks flips to descending). This is
// the "decreasing" half of First-Fit-Decreasing, but the strict-stop sweep
// still head-of-line blocks on the first job that cannot place; see Ljsfpack
// for the non-blocking variant that realizes true finite-bin FFD. Pure size
// policy: needs no duration estimator and leaves est_duration unset.
class Ljsf : public AdmissionPolicy {
  public:
    JobInstance* select_next(const std::vector<JobInstance*>& pending,
                             const ClusterView& view) const override;
    std::string name() const override {
        return "ljsf";
    }
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
