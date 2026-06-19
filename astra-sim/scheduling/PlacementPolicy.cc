/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/PlacementPolicy.hh"

#include "astra-sim/scheduling/BlockSelector.hh"
#include "astra-sim/scheduling/FirstFit.hh"
#include "astra-sim/scheduling/FragmentationScorer.hh"
#include "astra-sim/scheduling/L1Clustering.hh"
#include "astra-sim/scheduling/PlacementRanker.hh"
#include "astra-sim/scheduling/RFold.hh"
#include "astra-sim/scheduling/Random.hh"
#include "astra-sim/scheduling/SpaceFillingCurve.hh"
#include "astra-sim/scheduling/TopoMatch.hh"

namespace AstraSim {
namespace Scheduling {

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
    if (policy_name == "rfold") {
        auto scorer =
            make_fragmentation_scorer(cfg.defrag_metric, cfg.block_size);
        if (!scorer) {
            return nullptr;  // unknown --defrag-metric
        }
        auto selector =
            make_block_selector(cfg.rfold_selector, cfg.rfold_search_budget);
        if (!selector) {
            return nullptr;  // unknown --rfold-selector
        }
        auto ranker = make_placement_ranker(cfg.rfold_ranking, cfg);
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
