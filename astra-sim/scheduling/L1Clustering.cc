/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/L1Clustering.hh"

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace AstraSim {
namespace Scheduling {

namespace {

inline std::array<int, 3> decode_id(int id, int W, int L) {
    const int z = id / (L * W);
    const int rem = id % (L * W);
    const int y = rem / W;
    const int x = rem % W;
    return {x, y, z};
}

inline int torus_l1(
    int ax, int ay, int az, int bx, int by, int bz, int W, int L, int H) {
    const int dx = std::abs(ax - bx);
    const int dy = std::abs(ay - by);
    const int dz = std::abs(az - bz);
    return std::min(dx, W - dx) + std::min(dy, L - dy) + std::min(dz, H - dz);
}

}  // namespace

PlacementResult L1Clustering::try_place(const JobInstance& job,
                                        const ClusterView& view) {
    PlacementResult r;
    const auto& dims = view.physical_dims();
    if (dims.size() != 3) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "L1Clustering requires 3-D physical_dims (--npus-per-dim)";
        return r;
    }
    const int W = dims[0];
    const int L = dims[1];
    const int H = dims[2];

    if (job.num_ranks > view.total_npus()) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "num_ranks exceeds total NPUs";
        return r;
    }

    const auto& free = view.free_npus();
    const int n_free = static_cast<int>(free.size());
    if (n_free < job.num_ranks) {
        r.outcome = PlacementOutcome::DEFER;
        r.reason = "fewer free NPUs than num_ranks";
        return r;
    }

    const int k = job.num_ranks;

    // Decode coordinates of all free NPUs once.
    struct Entry {
        int id;
        int x, y, z;
    };
    std::vector<Entry> entries;
    entries.reserve(n_free);
    for (int id : free) {
        auto c = decode_id(id, W, L);
        entries.push_back({id, c[0], c[1], c[2]});
    }

    // Sample candidate centers and find the one that minimizes the total
    // L1 torus distance to its k closest free NPUs.
    const int step = std::max(1, n_free / 100);

    int best_cost = std::numeric_limits<int>::max();
    int best_center_id = std::numeric_limits<int>::max();
    std::vector<std::pair<int, int>> best_selection;  // (dist, id)

    std::vector<std::pair<int, int>> scored;  // reused across iterations
    scored.resize(n_free);

    for (int ci = 0; ci < n_free; ci += step) {
        const auto& ctr = entries[ci];

        // Compute distances from this center to all free NPUs.
        for (int j = 0; j < n_free; ++j) {
            const auto& e = entries[j];
            scored[j] = {torus_l1(ctr.x, ctr.y, ctr.z, e.x, e.y, e.z, W, L, H),
                         e.id};
        }

        // Partition so the first k elements are the k smallest by
        // (distance, id).
        std::nth_element(scored.begin(), scored.begin() + k - 1, scored.end());

        // Sum distances of the k closest.
        int cost = 0;
        for (int j = 0; j < k; ++j) {
            cost += scored[j].first;
        }

        if (cost < best_cost ||
            (cost == best_cost && ctr.id < best_center_id)) {
            best_cost = cost;
            best_center_id = ctr.id;
            best_selection.assign(scored.begin(), scored.begin() + k);
        }
    }

    // Sort the winning selection by (distance, id) for deterministic rank
    // ordering.
    std::sort(best_selection.begin(), best_selection.end());

    r.outcome = PlacementOutcome::PLACED;
    r.npus.resize(k);
    for (int i = 0; i < k; ++i) {
        r.npus[i] = best_selection[i].second;
    }
    return r;
}

}  // namespace Scheduling
}  // namespace AstraSim
