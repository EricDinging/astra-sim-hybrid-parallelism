/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/SpaceFillingCurve.hh"

#include "astra-sim/scheduling/Common.hh"

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/HilbertCurve.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include <algorithm>
#include <cstdint>
#include <utility>

// SFC (Hilbert) placement on a 3-D torus.
//
// The Hilbert curve operates in a 2^p x 2^p x 2^p embedding cube, where
// p = ceil(log2(max(W, L, H))). For non-cube clusters the embedding cube
// strictly contains the cluster; "phantom" cells outside [0,W)x[0,L)x[0,H)
// never appear in view.free_npus(), so they're transparent to the policy.

namespace AstraSim {
namespace Scheduling {

namespace {

inline int ceil_log2_int(int n) {
    if (n <= 1) {
        return 0;
    }
    int p = 0;
    int v = n - 1;
    while (v > 0) {
        ++p;
        v >>= 1;
    }
    return p;
}

}  // namespace

PlacementResult SpaceFillingCurve::try_place(const JobInstance& job,
                                             const ClusterView& view) {
    if (auto guard = basic_precheck(job, view, "SFC")) {
        return *guard;
    }
    PlacementResult r;
    const auto& dims = view.physical_dims();
    const int W = dims[0];
    const int L = dims[1];
    const int H = dims[2];

    const int p = ceil_log2_int(std::max({W, L, H}));
    if (p > 21) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "cluster axis exceeds SFC embedding precision";
        return r;
    }

    const auto& free = view.free_npus();

    std::vector<std::pair<uint64_t, int>> scored;
    scored.reserve(free.size());
    for (int id : free) {
        const auto xyz = coord_of(id, W, L);
        const uint64_t d = hilbert_d_from_xyz(xyz[0], xyz[1], xyz[2], p);
        scored.emplace_back(d, id);
    }
    std::sort(scored.begin(), scored.end());

    r.outcome = PlacementOutcome::PLACED;
    r.npus.resize(job.num_ranks);
    for (int i = 0; i < job.num_ranks; ++i) {
        r.npus[i] = scored[static_cast<std::size_t>(i)].second;
    }
    return r;
}

}  // namespace Scheduling
}  // namespace AstraSim
