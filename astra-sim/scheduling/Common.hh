/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_COMMON_HH__
#define __ASTRASIM_SCHEDULING_COMMON_HH__

#include "astra-sim/common/Common.hh"
#include "astra-sim/scheduling/ReconfigPlan.hh"

#include <array>
#include <string>
#include <vector>

namespace AstraSim {
namespace Scheduling {

enum class JobStatus { PENDING, RUNNING, COMPLETED, DROPPED, DEFER_AT_EXIT };

// Decode a global NPU id into (x, y, z) torus coords; id = z*L*W + y*W + x
// with x the fastest-varying axis (matches FirstFit placement and DOR
// routing). W and L are the sizes of the first two torus dimensions.
inline std::array<int, 3> coord_of(int id, int W, int L) {
    return {id % W, (id / W) % L, id / (W * L)};
}

enum class PlacementOutcome { PLACED, DEFER, DROP };

struct PlacementResult {
    PlacementOutcome outcome;
    std::vector<int> npus;
    std::string reason;
    // When true, the collective ring for this job is built in rank_map order
    // instead of sorted-id order (Decision D4). Only the rfold policy sets
    // this; every other policy leaves it false (byte-identical behavior).
    bool ordered_rings = false;
    // Owned OCS links + routes for this job (rfold only); empty for every other
    // policy. SchedRuntime applies it via a ReconfigHook before firing
    // workloads.
    ReconfigPlan reconfig_plan;
};

struct PlacementConfig {
    std::string defrag_metric = "fewest-blocks";  // --defrag-metric
    std::array<int, 3> block_size = {4, 4, 4};    // --block-size (rfold)
    std::string rfold_selector = "min-reconfig";  // --rfold-selector
    std::string rfold_ranking = "frag-first";     // --rfold-ranking
    int rfold_search_budget = 50000;              // --rfold-search-budget
    bool rfold_multifold = true;                  // --rfold-multifold
    int switch_theta = 1;  // DynamicSwitch backlog threshold
};

struct JobArrival {
    int job_id;
    Tick arrival_time;
    int num_ranks;
    std::array<int, 3> shape;
    // Number of training iterations to replay for this job (>= 1). Optional in
    // the arrival CSV; defaults to 1 (single-iteration, legacy behavior).
    int num_iterations = 1;
};

std::string to_string(JobStatus s);

}  // namespace Scheduling
}  // namespace AstraSim

#endif
