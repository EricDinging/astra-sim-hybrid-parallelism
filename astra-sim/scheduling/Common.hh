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
    std::string defrag_metric = "fewest-blocks-ocs";  // --defrag-metric
    std::array<int, 3> block_size = {4, 4, 4};        // --block-size (rfold)
    std::string rfold_selector = "min-reconfig";      // --rfold-selector
    std::string rfold_ranking =
        "auto";  // --rfold-ranking (auto: per rfold version)
    int rfold_search_budget = 50000;  // --rfold-search-budget
    bool rfold_multifold = true;      // --rfold-multifold
    // cost-model ranking knobs (--cost-model-*), spec 2026-06-11 §4
    double cm_kappa = 1.0;           // seam-penalty calibration
    double cm_c_ext = 1.0;           // externality coefficient
    double cm_t_reconfig_s = 0.010;  // per-OCS-link charge (seconds)
    double cm_gamma = 1.0;           // lookahead charge coefficient
    int cm_lookahead_k = 0;          // top-K finalists probed (0 = proxy only)
    int cm_lookahead_q = 8;          // queued shapes probed per finalist
    bool cm_scatter_guard = true;    // keep scatter strictly last
    // >0: freeze the proxy externality's beta to this constant (seconds),
    // replacing the state-dependent c_ext*Q*mean(t_svc) — the spec's
    // "cost-model-static" ablation arm. 0 = dynamic beta (default).
    double cm_beta_const_s = 0.0;
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
