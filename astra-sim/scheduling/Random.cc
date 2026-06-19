/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/Random.hh"

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include <algorithm>

namespace AstraSim {
namespace Scheduling {

Random::Random() : rng_(42) {}

PlacementResult Random::try_place(const JobInstance& job,
                                  const ClusterView& view) {
    PlacementResult r;
    if (job.num_ranks > view.total_npus()) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "num_ranks exceeds total NPUs";
        return r;
    }
    const auto& free = view.free_npus();
    if (static_cast<int>(free.size()) < job.num_ranks) {
        r.outcome = PlacementOutcome::DEFER;
        r.reason = "fewer free NPUs than num_ranks";
        return r;
    }

    std::vector<int> pool(free.begin(), free.end());
    std::shuffle(pool.begin(), pool.end(), rng_);
    pool.resize(job.num_ranks);

    r.outcome = PlacementOutcome::PLACED;
    r.npus = std::move(pool);
    return r;
}

}  // namespace Scheduling
}  // namespace AstraSim
