/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/PlacementRanker.hh"

#include <algorithm>
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
        // v2 leads the scatter class with the RDCN damage metric: newly-
        // wounded clean blocks. v1 key unchanged for bit-compat.
        if (!v1_keys_) {
            return std::make_tuple(a.dirty_delta, a.blocks_touched, a.dor_edges,
                                   a.cost) <
                   std::make_tuple(b.dirty_delta, b.blocks_touched, b.dor_edges,
                                   b.cost);
        }
        return std::make_tuple(a.blocks_touched, a.dor_edges, a.cost) <
               std::make_tuple(b.blocks_touched, b.dor_edges, b.cost);
    }
    const auto key = [&](const Placement& p) {
        return std::make_tuple(p.dor_edges, p.blocks_touched,
                               v1_keys_ ? p.ocs_links() : 0,
                               v1_keys_ ? p.residual_frag : 0, p.cost);
    };
    return key(a) < key(b);
}

bool PackingFirst::better(const Placement& a, const Placement& b) const {
    auto eff = [](const Placement& p) {
        return p.cost + ((!p.ring_closes && !p.scattered) ? 0.5 : 0.0);
    };
    return std::make_tuple(a.blocks_touched, a.dor_edges, eff(a)) <
           std::make_tuple(b.blocks_touched, b.dor_edges, eff(b));
}

double CostModelRanker::cost_of(const Placement& p) const {
    double est = ctx_ != nullptr ? ctx_->current_est_duration_s : 0.0;
    int ring =
        ctx_ != nullptr ? std::max(1, ctx_->current_total_ring_edges) : 1;
    const double comm = kappa_ * p.dor_edges * est / ring;
    double ext = p.lookahead_ext_s;
    if (lookahead_k_ == 0 && p.blocks_touched > 0) {
        const double dfrag = static_cast<double>(p.residual_frag) /
                             (6.0 * static_cast<double>(p.blocks_touched));
        if (beta_const_s_ > 0.0) {
            // Frozen beta ("cost-model-static" arm): externality is
            // state-independent by construction.
            ext += beta_const_s_ * dfrag;
        } else if (ctx_ != nullptr && ctx_->queue_depth > 0 &&
                   !ctx_->queued.empty()) {
            double t_sum = 0.0;
            for (const auto& q : ctx_->queued) {
                t_sum += q.est_duration_s;
            }
            const double t_svc =
                t_sum / static_cast<double>(ctx_->queued.size());
            ext +=
                c_ext_ * static_cast<double>(ctx_->queue_depth) * t_svc * dfrag;
        }
    }
    const double rcfg = t_reconfig_s_ * static_cast<double>(p.ocs_links());
    return comm + ext + rcfg;
}

bool CostModelRanker::better(const Placement& a, const Placement& b) const {
    if (guard_ && a.scattered != b.scattered) {
        return !a.scattered;
    }
    return cost_of(a) < cost_of(b);
}

std::unique_ptr<PlacementRanker> make_placement_ranker(
    const std::string& name, const PlacementConfig& cfg) {
    if (name == "comm-first") {
        return std::make_unique<CommFirst>(/*v1_keys=*/true);
    }
    if (name == "comm-first-no-residual") {
        return std::make_unique<CommFirst>(/*v1_keys=*/false);
    }
    if (name == "packing-first") {
        return std::make_unique<PackingFirst>();
    }
    if (name == "cost-model") {
        return std::make_unique<CostModelRanker>(cfg);
    }
    if (name == "switch") {
        return std::make_unique<DynamicSwitch>(cfg.switch_theta);
    }
    return nullptr;
}

}  // namespace Scheduling
}  // namespace AstraSim
