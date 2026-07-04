/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_EASY_HH__
#define __ASTRASIM_SCHEDULING_EASY_HH__

#include "astra-sim/scheduling/AdmissionPolicy.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"

#include <memory>

namespace AstraSim {
namespace Scheduling {

// EASY (aggressive, one-reservation) backfilling admission. Queue order is pure
// FCFS; the backfill logic lives in SchedRuntime::easy_sweep, dispatched via
// uses_backfill() == true. Like sjdf/swf, EASY owns a DurationEstimator and
// caches est_duration for every job in on_arrival -- the reservation
// arithmetic and the projected completion of running jobs both need durations.
// select_next is a pure-FCFS fallback; it is never called on the normal EASY
// path (SchedRuntime dispatches to easy_sweep instead).
class Easy : public DurationKeyedPolicy {
  public:
    using DurationKeyedPolicy::DurationKeyedPolicy;
    // Pure FCFS head; a harmless fallback unused on the EASY path (SchedRuntime
    // dispatches to easy_sweep instead of select_next).
    JobInstance* select_next(const std::vector<JobInstance*>& pending,
                             const ClusterView& view) const override;
    bool uses_backfill() const override {
        return true;
    }
    std::string name() const override {
        return "easy";
    }
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
