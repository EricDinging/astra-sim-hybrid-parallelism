/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_EASYSHAPE_HH__
#define __ASTRASIM_SCHEDULING_EASYSHAPE_HH__

#include "astra-sim/scheduling/Easy.hh"

namespace AstraSim {
namespace Scheduling {

// Shape-aware EASY backfilling. Same FCFS queue and one-reservation rule as
// Easy, but the reservation is the pivot's actual placement: SchedRuntime
// probes the placement policy against the free set plus the earliest-ending
// running jobs and reserves the NPUs of the first placement found. Backfill
// candidates that outlive the shadow time are then placed with those NPUs
// removed from the view, so the pivot's region can never be re-fragmented
// by backfill. Fixes count-based EASY's starvation on a torus, where the
// pivot "fits by count" indefinitely while its contiguous shape never
// materializes (see SchedRuntime::easy_sweep).
class EasyShape : public Easy {
  public:
    using Easy::Easy;
    bool shape_aware_reservation() const override {
        return true;
    }
    std::string name() const override {
        return "easyshape";
    }
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
