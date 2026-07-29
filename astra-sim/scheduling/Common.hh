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
    // True when rfold placed this job by relaxing the fold-shape constraint
    // (scatter fallback, all edges on standard DOR). SchedRuntime copies it to
    // the JobInstance so the concurrently-relaxed footprint can be budgeted.
    bool relaxed = false;
    // Link-load price of a relaxed placement: total unidirectional-DOR hops
    // its comm-ring edges occupy on the shared fabric. A folded job's owned
    // 1-hop edges cost 0. Denominates the relaxation budget in the units the
    // damage actually occurs in (a scattered ring edge on an 8^3 torus rides
    // ~12 links, not 1 -- rank-count budgets underprice scatter by that
    // factor).
    int relax_link_load = 0;
};

struct PlacementConfig {
    std::string defrag_metric = "fewest-blocks";  // --defrag-metric
    std::array<int, 3> block_size = {4, 4, 4};    // --block-size (rfold)
    std::string rfold_selector = "min-reconfig";  // --rfold-selector
    std::string rfold_ranking = "frag-first";     // --rfold-ranking
    int rfold_search_budget = 50000;              // --rfold-search-budget
    bool rfold_multifold = true;                  // --rfold-multifold
    int switch_theta = 1;  // DynamicSwitch backlog threshold
    // Shape-constraint relaxation, master switch (--rfold-relax): a job no
    // fold variant places right now, after waiting >= min_wait, takes a
    // reconfiguration-aware degraded placement (brick-split slabs with
    // OCS-wired seams, then SFC scatter). Need + stall floor are the whole
    // gate: full-length bidi validation (design doc 10) showed every extra
    // cap (link-load budget, stretch floor, clustered fallback) is neutral
    // or harmful, so they were removed. DEFAULT OFF: on the unidirectional
    // fabric the feature is measurably harmful at sustained load (~+2%
    // mean JCT, doc 6.2); enable it with --bidi, where it wins up to 11x
    // at the capacity knee and beats sfc at saturation.
    bool rfold_relax = false;  // --rfold-relax
    // Stall floor: a job may relax only after waiting at least this many
    // seconds -- the one load-bearing knob (doc 10): low-load queues never
    // reach it (provably inert), stalled queues pass it quickly.
    double rfold_relax_min_wait = 5.0;  // --rfold-relax-min-wait
    // Mirrors --bidi (routing-layer flag): relaxed residual edges then ride
    // the shorter arc per dimension, so RFold prices link load in min-arc
    // hops and doubles the budget denominator (6N directed links, not 3N).
    bool bidi = false;
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
