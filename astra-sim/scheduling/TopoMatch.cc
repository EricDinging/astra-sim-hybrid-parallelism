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
    if (auto guard = basic_precheck(job, view, "topomatch")) {
        return *guard;
    }
    PlacementResult r;
    const auto& dims = view.physical_dims();
    const auto& free = view.free_npus();

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
