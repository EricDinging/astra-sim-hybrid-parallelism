/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/FootprintRouter.hh"

#include "astra-sim/scheduling/Common.hh"

#include <algorithm>
#include <set>

namespace AstraSim {
namespace Scheduling {

std::vector<std::pair<int, int>> FootprintRouter::ring_edges(
    const std::array<int, 3>& shape) {
    const int sizes[3] = {shape[0], shape[1], shape[2]};
    const int strides[3] = {1, shape[0], shape[0] * shape[1]};
    std::set<std::pair<int, int>> seen;
    std::vector<std::pair<int, int>> out;
    auto add = [&](int a, int b) {
        if (a == b) {
            return;
        }
        if (seen.insert({a, b}).second) {
            out.push_back({a, b});
        }
        if (seen.insert({b, a}).second) {
            out.push_back({b, a});
        }
    };
    // DP-fastest linearization: i = pp*(s0*s1) + tp*s0 + dp. For each
    // parallelism dim with size > 1, connect each rank to the next rank along
    // that dim (with the ring closure) -- the actual collective comm pattern.
    for (int pp = 0; pp < sizes[2]; ++pp) {
        for (int tp = 0; tp < sizes[1]; ++tp) {
            for (int dp = 0; dp < sizes[0]; ++dp) {
                const int i = pp * strides[2] + tp * strides[1] + dp;
                const int idx[3] = {dp, tp, pp};
                for (int d = 0; d < 3; ++d) {
                    if (sizes[d] <= 1) {
                        continue;
                    }
                    const int next = (idx[d] + 1) % sizes[d];
                    add(i, i + (next - idx[d]) * strides[d]);
                }
            }
        }
    }
    return out;
}

std::vector<std::pair<int, int>> FootprintRouter::ocs_edges(
    const std::vector<std::pair<int, int>>& edges,
    const std::vector<int>& rank_map,
    const BlockModel& cm) {
    std::set<std::pair<int, int>> uniq;
    for (const auto& e : edges) {
        int u = rank_map[e.first];
        int v = rank_map[e.second];
        if (!cm.is_torus_adjacent(u, v)) {
            uniq.insert({std::min(u, v), std::max(u, v)});
        }
    }
    return {uniq.begin(), uniq.end()};
}

std::vector<RouteRow> FootprintRouter::adjacent_routes(
    const std::vector<std::pair<int, int>>& edges,
    const std::vector<int>& rank_map,
    const std::vector<int>& dims,
    const std::vector<std::pair<int, int>>& ocs_edges) {
    const int Dx = dims[0], Dy = dims[1];
    auto id_to_coord = [&](int n) { return coord_of(n, Dx, Dy); };
    const std::set<std::pair<int, int>> ocs(ocs_edges.begin(), ocs_edges.end());
    std::vector<RouteRow> rows;
    rows.reserve(edges.size());
    for (const auto& e : edges) {
        const int u = rank_map[e.first];
        const int v = rank_map[e.second];
        const auto cu = id_to_coord(u);
        const auto cv = id_to_coord(v);
        int hop_dist = 0;
        for (int d = 0; d < 3; ++d) {
            const int D = dims[d];
            const int forward = ((cv[d] - cu[d]) % D + D) % D;
            hop_dist += std::min(forward, D - forward);
        }
        const bool wired = ocs.count({std::min(u, v), std::max(u, v)}) > 0;
        if (hop_dist == 1 || wired) {  // a direct link exists; pin it
            rows.push_back(RouteRow{u, v, {u, v}});
        }
        // Ring edges that are neither torus-adjacent nor OCS-served (identity
        // partial-axis closures, open-fold seams) are left unpinned -- they
        // follow the backend's standard DOR.
    }
    return rows;
}

}  // namespace Scheduling
}  // namespace AstraSim
