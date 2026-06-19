/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_SCHEDCONTEXT_HH__
#define __ASTRASIM_SCHEDULING_SCHEDCONTEXT_HH__

#include <array>
#include <functional>
#include <unordered_set>
#include <vector>

namespace AstraSim {
namespace Scheduling {

struct QueuedJobInfo {
    std::array<int, 3> shape;
    int num_ranks = 0;
    double est_duration_s = 0.0;  // 0 when no estimator ran
};

// True iff shape/num_ranks can place on the hypothetical free set.
using ProbeOracle = std::function<bool(const std::array<int, 3>& shape,
                                       int num_ranks,
                                       const std::unordered_set<int>& free)>;

// Cluster/queue state for state-aware rankers (cost-model, switch). Built
// fresh by SchedRuntime before each try_place; RFold adds the current-job
// fields and installs it on its ranker for the duration of the call only.
struct SchedContext {
    int queue_depth = 0;  // pending jobs EXCLUDING the placing one
    std::vector<QueuedJobInfo> queued;  // FCFS order, head first
    // Set by RFold per decision:
    double current_est_duration_s = 0.0;
    int current_total_ring_edges = 0;
    // Set by RFold when lookahead is on; tests may inject their own.
    ProbeOracle probe;
};

}  // namespace Scheduling
}  // namespace AstraSim
#endif
