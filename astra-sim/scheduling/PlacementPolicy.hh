/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_PLACEMENTPOLICY_HH__
#define __ASTRASIM_SCHEDULING_PLACEMENTPOLICY_HH__

#include "astra-sim/scheduling/Common.hh"

#include <memory>
#include <string>

namespace AstraSim {
namespace Scheduling {

class JobInstance;
class ClusterView;
struct SchedContext;

// Placement policy: decides WHERE a job runs (which NPUs).
class PlacementPolicy {
  public:
    virtual ~PlacementPolicy() = default;
    virtual PlacementResult try_place(const JobInstance& job,
                                      const ClusterView& view) = 0;
    virtual std::string name() const = 0;
    // Per-decision cluster/queue state from SchedRuntime; nullptr clears.
    // Policies that don't rank on state ignore it.
    virtual void set_sched_context(const SchedContext* ctx) {
        (void)ctx;
    }
    // True iff set_sched_context is actually consumed (rfold with a
    // context-aware ranker). SchedRuntime skips building the context -- a
    // copy + sort of the whole pending queue per placement attempt -- when
    // this is false.
    virtual bool wants_sched_context() const {
        return false;
    }
};

// Returns nullptr on unknown name (caller hard-exits with a clearer message).
std::unique_ptr<PlacementPolicy> make_placement_policy(
    const std::string& policy_name,
    const PlacementConfig& cfg = PlacementConfig{});

}  // namespace Scheduling
}  // namespace AstraSim

#endif
