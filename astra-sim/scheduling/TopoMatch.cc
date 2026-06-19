/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/TopoMatch.hh"

#include "astra-sim/scheduling/AffinityMatrix.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/JobInstance.hh"

namespace AstraSim {
namespace Scheduling {

PlacementResult TopoMatch::try_place(const JobInstance& job,
                                     const ClusterView& view) {
    PlacementResult r;
    const auto& dims = view.physical_dims();
    if (dims.size() != 3) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "topomatch requires 3-D physical_dims (--npus-per-dim)";
        return r;
    }
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

    auto affinity = build_affinity_matrix(job.trace_dir, job.num_ranks);
    if (!affinity) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "cannot build affinity matrix from traces";
        return r;
    }

    if (!solver_ || sw_ != dims[0] || sl_ != dims[1] || sh_ != dims[2]) {
        sw_ = dims[0];
        sl_ = dims[1];
        sh_ = dims[2];
        solver_ = std::make_unique<TopoMatchSolver>(sw_, sl_, sh_);
    }

    auto sigma = solver_->solve(free, *affinity);
    if (!sigma) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "topomatch solver failed";
        return r;
    }

    r.outcome = PlacementOutcome::PLACED;
    r.npus = std::move(*sigma);
    return r;
}

}  // namespace Scheduling
}  // namespace AstraSim
