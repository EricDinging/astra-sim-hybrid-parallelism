/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_RECONFIGPLAN_HH__
#define __ASTRASIM_SCHEDULING_RECONFIGPLAN_HH__

#include <utility>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// One installed route as an NPU-id sequence; hops.front()==src,
// hops.back()==dst.
struct RouteRow {
    int src;
    int dst;
    std::vector<int> hops;
};

// The OCS links a placed job owns plus the routes to install for it. Produced
// by the rfold policy in PlacementResult; applied by SchedRuntime via a
// ReconfigHook.
struct ReconfigPlan {
    std::vector<std::pair<int, int>>
        ocs_edges;                 // global (u, v) OCS links to wire
    std::vector<RouteRow> routes;  // id-sequence routes to install
    bool empty() const {
        return ocs_edges.empty() && routes.empty();
    }
};

// Seam: SchedRuntime calls apply() on PLACED; the frontend implements it over
// the backend TopologyManager. Keeps scheduling decoupled from the backend.
class ReconfigHook {
  public:
    virtual ~ReconfigHook() = default;
    virtual void apply(const ReconfigPlan& plan) = 0;
};

}  // namespace Scheduling
}  // namespace AstraSim
#endif
