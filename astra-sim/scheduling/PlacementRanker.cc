/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/PlacementRanker.hh"

#include <tuple>

namespace AstraSim {
namespace Scheduling {

namespace {
int comm_class(const Placement& p) {
    if (p.scattered) {
        return 2;
    }
    return p.dor_edges == 0 ? 0 : 1;
}
}  // namespace

bool CommFirst::better(const Placement& a, const Placement& b) const {
    const int ca = comm_class(a);
    const int cb = comm_class(b);
    if (ca != cb) {
        return ca < cb;
    }
    if (ca == 2) {
        // Scatter class leads with the RDCN damage metric: newly-fragmented
        // idle blocks.
        return std::make_tuple(a.frag_delta, a.blocks_touched, a.dor_edges,
                               a.cost) < std::make_tuple(b.frag_delta,
                                                         b.blocks_touched,
                                                         b.dor_edges, b.cost);
    }
    return std::make_tuple(a.dor_edges, a.blocks_touched, a.cost) <
           std::make_tuple(b.dor_edges, b.blocks_touched, b.cost);
}

bool FragFirst::better(const Placement& a, const Placement& b) const {
    return std::make_tuple(a.frag_delta, a.blocks_touched, a.dor_edges,
                           a.cost) < std::make_tuple(b.frag_delta,
                                                     b.blocks_touched,
                                                     b.dor_edges, b.cost);
}

bool PackingFirst::better(const Placement& a, const Placement& b) const {
    auto eff = [](const Placement& p) {
        return p.cost + ((!p.ring_closes && !p.scattered) ? 0.5 : 0.0);
    };
    return std::make_tuple(a.blocks_touched, a.dor_edges, eff(a)) <
           std::make_tuple(b.blocks_touched, b.dor_edges, eff(b));
}

std::unique_ptr<PlacementRanker> make_placement_ranker(
    const std::string& name, const PlacementConfig& cfg) {
    if (name == "comm-first") {
        return std::make_unique<CommFirst>();
    }
    if (name == "frag-first") {
        return std::make_unique<FragFirst>();
    }
    if (name == "packing-first") {
        return std::make_unique<PackingFirst>();
    }
    if (name == "switch") {
        return std::make_unique<DynamicSwitch>(cfg.switch_theta);
    }
    return nullptr;
}

}  // namespace Scheduling
}  // namespace AstraSim
