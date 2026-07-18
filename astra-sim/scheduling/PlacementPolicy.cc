/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/PlacementPolicy.hh"

#include "astra-sim/scheduling/BlockSelector.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/FirstFit.hh"
#include "astra-sim/scheduling/FragmentationScorer.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/L1Clustering.hh"
#include "astra-sim/scheduling/PlacementRanker.hh"
#include "astra-sim/scheduling/RFold.hh"
#include "astra-sim/scheduling/Random.hh"
#include "astra-sim/scheduling/SpaceFillingCurve.hh"
#include "astra-sim/scheduling/TopoMatch.hh"

namespace AstraSim {
namespace Scheduling {

std::optional<PlacementResult> basic_precheck(const JobInstance& job,
                                              const ClusterView& view,
                                              const std::string& policy_name) {
    PlacementResult r;
    if (view.physical_dims().size() != 3) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = policy_name + " requires 3-D physical_dims (--npus-per-dim)";
        return r;
    }
    if (job.num_ranks > view.total_npus()) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "num_ranks exceeds total NPUs";
        return r;
    }
    if (static_cast<int>(view.free_npus().size()) < job.num_ranks) {
        r.outcome = PlacementOutcome::DEFER;
        r.reason = "fewer free NPUs than num_ranks";
        return r;
    }
    return std::nullopt;
}

std::unique_ptr<PlacementPolicy> make_placement_policy(
    const std::string& policy_name, const PlacementConfig& cfg) {
    if (policy_name == "firstfit") {
        return std::make_unique<FirstFit>();
    }
    if (policy_name == "random") {
        return std::make_unique<Random>();
    }
    if (policy_name == "sfc") {
        return std::make_unique<SpaceFillingCurve>();
    }
    if (policy_name == "l1clustering") {
        return std::make_unique<L1Clustering>();
    }
    if (policy_name == "topomatch") {
        return std::make_unique<TopoMatch>();
    }
    // rfoldv2 (default; `rfold` is an alias for it) and rfoldv1 share every
    // moving part; the version only picks the two knobs the 2026-07-18
    // investigation changed:
    //   v1: comm-first ranking (wall-hugging residual key) + orientation-
    //       locked scatter — bit-compatible with pre-2026-07 rfold sweeps
    //   v2: comm-first-no-residual + scatter tries all 6 rotations
    // An explicit --rfold-ranking overrides either version's default.
    if (policy_name == "rfold" || policy_name == "rfoldv1" ||
        policy_name == "rfoldv2") {
        const bool v2 = policy_name != "rfoldv1";
        auto scorer =
            make_fragmentation_scorer(cfg.defrag_metric, cfg.block_size);
        if (!scorer) {
            return nullptr;  // unknown --defrag-metric
        }
        auto selector = make_block_selector(
            cfg.rfold_selector, cfg.rfold_search_budget, /*rotate_scatter=*/v2);
        if (!selector) {
            return nullptr;  // unknown --rfold-selector
        }
        const std::string ranking =
            cfg.rfold_ranking != "auto"
                ? cfg.rfold_ranking
                : (v2 ? "comm-first-no-residual" : "comm-first");
        auto ranker = make_placement_ranker(ranking, cfg);
        if (!ranker) {
            return nullptr;  // unknown --rfold-ranking
        }
        return std::make_unique<RFold>(cfg.block_size, std::move(selector),
                                       std::move(scorer), std::move(ranker),
                                       cfg);
    }
    return nullptr;
}

}  // namespace Scheduling
}  // namespace AstraSim
