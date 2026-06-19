/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_LJSFPACK_HH__
#define __ASTRASIM_SCHEDULING_LJSFPACK_HH__

#include "astra-sim/scheduling/Ljsf.hh"

#include <string>

namespace AstraSim {
namespace Scheduling {

// Largest-job-size-first admission with NON-BLOCKING (greedy) admission: the
// same descending-size ordering as Ljsf, but a job that cannot be placed now is
// SKIPPED rather than blocking the queue head. SchedRuntime::greedy_sweep walks
// the pending set in descending size and admits every job that fits, realizing
// true finite-bin First-Fit-Decreasing when paired with FirstFit placement.
// (Ljsf supplies only the "decreasing" order; its strict-stop sweep still
// head-of-line blocks on the first unplaceable job.) Pure size policy: needs no
// duration estimator and leaves est_duration unset.
class Ljsfpack : public Ljsf {
  public:
    // Opts this policy into SchedRuntime::greedy_sweep (skip, don't block).
    bool skips_on_defer() const override {
        return true;
    }
    // Out-of-line to anchor the vtable here (matches the file-per-policy
    // convention; Ljsf's key function lives in Ljsf.cc).
    std::string name() const override;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
