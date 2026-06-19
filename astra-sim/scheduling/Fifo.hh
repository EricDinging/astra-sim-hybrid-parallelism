/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_FIFO_HH__
#define __ASTRASIM_SCHEDULING_FIFO_HH__

#include "astra-sim/scheduling/AdmissionPolicy.hh"

namespace AstraSim {
namespace Scheduling {

// First-in-first-out admission: earliest (arrival_time, job_id) wins.
class Fifo : public AdmissionPolicy {
  public:
    JobInstance* select_next(const std::vector<JobInstance*>& pending,
                             const ClusterView& view) const override;
    std::string name() const override {
        return "fifo";
    }
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
