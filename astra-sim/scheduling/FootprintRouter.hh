/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_FOOTPRINTROUTER_HH__
#define __ASTRASIM_SCHEDULING_FOOTPRINTROUTER_HH__

#include "astra-sim/scheduling/BlockModel.hh"
#include "astra-sim/scheduling/ReconfigPlan.hh"  // RouteRow

#include <array>
#include <utility>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// Computes per-job routing from a fold embedding -- the RFold routing path,
// separate from the backend's DOR. ASTRA-sim collectives are ring-based, so
// every send is to a footprint-neighbor and collapses to a 1-hop route.
class FootprintRouter {
  public:
    // Ordered logical comm-ring edges as rank pairs (both directions) for the
    // job shape (DP, TP, PP) under DP-fastest linearization
    // i = pp*(s0*s1) + tp*s0 + dp: for each parallelism dim with size > 1,
    // consecutive ranks plus the ring closure. These are the edges the
    // collective actually sends on -- variant-independent (the fold only
    // changes which global NPU each rank maps to, via rank_map, not which ranks
    // communicate). Map through rank_map for adjacent_routes()/ocs_edges().
    static std::vector<std::pair<int, int>> ring_edges(
        const std::array<int, 3>& shape);

    // Deduped global OCS edges (stored u < v) among the ring edges: the ring
    // edges whose endpoints are NOT one torus hop apart, i.e. have no baseline
    // link and must be wired as real 1-hop OCS links (cross-block hops AND
    // same-block "self-wrap" closures of full-block-axis rings).
    static std::vector<std::pair<int, int>> ocs_edges(
        const std::vector<std::pair<int, int>>& edges,
        const std::vector<int>& rank_map,
        const BlockModel& cm);

    // Pin a 1-hop route for every ring edge whose endpoints land on
    // torus-adjacent NPUs (every edge of a ring-closing fold's hairpin cycle,
    // both directions of each edge) or whose endpoint pair is wired as an OCS
    // link (`ocs_edges`, stored u < v; empty when the placement wires no OCS,
    // e.g. at whole-torus block size). Ring edges that are neither adjacent
    // nor OCS-served (identity partial-axis ring closures, open-fold seams)
    // are intentionally not pinned: that traffic falls through to the
    // backend's standard DOR route, like any other policy's. `dims` are the
    // torus dims (x fastest, id = z*Dy*Dx + y*Dx + x).
    static std::vector<RouteRow> adjacent_routes(
        const std::vector<std::pair<int, int>>& edges,
        const std::vector<int>& rank_map,
        const std::vector<int>& dims,
        const std::vector<std::pair<int, int>>& ocs_edges = {});
};

}  // namespace Scheduling
}  // namespace AstraSim
#endif
