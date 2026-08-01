/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_PLACEMENTPOLICY_HH__
#define __ASTRASIM_SCHEDULING_PLACEMENTPOLICY_HH__

#include "astra-sim/scheduling/Common.hh"

#include <memory>
#include <optional>
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
    // True iff a DEFER outcome is a pure, side-effect-free function of
    // (job shape, free set) AND monotone in the free set (a smaller free set
    // can never flip DEFER to PLACED). SchedRuntime then memoizes DEFER per
    // shape and skips re-running the search until a job completion frees
    // NPUs. Opt-in per policy: randomized policies (RNG consumed per call)
    // and policies with time-dependent outcomes or in-call side effects must
    // return false.
    virtual bool defer_is_shape_sticky() const {
        return false;
    }
};

// Common entry guards for 3-D torus placement policies: DROP for non-3-D
// physical_dims or a job larger than the cluster, DEFER when there are fewer
// free NPUs than ranks. Returns nullopt when all guards pass.
std::optional<PlacementResult> basic_precheck(const JobInstance& job,
                                              const ClusterView& view,
                                              const std::string& policy_name);

// Returns nullptr on unknown name (caller hard-exits with a clearer message).
std::unique_ptr<PlacementPolicy> make_placement_policy(
    const std::string& policy_name,
    const PlacementConfig& cfg = PlacementConfig{});

}  // namespace Scheduling
}  // namespace AstraSim

#endif
