/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/RFold.hh"

#include "astra-sim/scheduling/BlockModel.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/FoldEnumerator.hh"
#include "astra-sim/scheduling/FootprintRouter.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include <algorithm>
#include <array>
#include <cassert>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AstraSim {
namespace Scheduling {

namespace {
// A rotation of footprint `fp` fits `dims` iff sorted(fp) <= sorted(dims).
bool fits_cluster(std::array<int, 3> fp, const std::vector<int>& dims) {
    std::array<int, 3> d{dims[0], dims[1], dims[2]};
    std::sort(fp.begin(), fp.end());
    std::sort(d.begin(), d.end());
    return fp[0] <= d[0] && fp[1] <= d[1] && fp[2] <= d[2];
}

// 1-D snake DFS bound (folding's placement-time budget); shared by the
// placement-of-last-resort snake and the idle-cluster oracle.
constexpr int kSnakeBudget = 200000;
}  // namespace

namespace detail {
void apply_lookahead(std::vector<Placement>& cands,
                     int k,
                     int q,
                     double gamma,
                     const SchedContext& ctx,
                     const std::unordered_set<int>& free) {
    if (k <= 0) {
        return;
    }
    if (static_cast<int>(cands.size()) > k) {
        cands.resize(k);
    }
    for (auto& c : cands) {
        std::unordered_set<int> after(free);
        for (int n : c.rank_map) {
            after.erase(n);
        }
        double charge = 0.0;
        int probed = 0;
        for (const auto& jq : ctx.queued) {
            if (probed++ >= q) {
                break;
            }
            if (!ctx.probe(jq.shape, jq.num_ranks, free)) {
                continue;  // not placeable even now: nothing to flip
            }
            if (!ctx.probe(jq.shape, jq.num_ranks, after)) {
                charge += gamma * jq.est_duration_s;
            }
        }
        c.lookahead_ext_s = charge;
    }
}
}  // namespace detail

PlacementResult RFold::try_place(const JobInstance& job,
                                 const ClusterView& view) {
    PlacementResult r;
    const auto& dims = view.physical_dims();
    if (dims.size() != 3) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "rfold requires 3-D physical_dims (--npus-per-dim)";
        return r;
    }
    BlockModel cm({dims[0], dims[1], dims[2]}, block_);
    if (!cm.valid()) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "--block-size must divide every torus dim per axis";
        return r;
    }
    if (job.num_ranks > view.total_npus()) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "num_ranks exceeds cluster size";
        return r;
    }

    std::unordered_set<int> free(view.free_npus().begin(),
                                 view.free_npus().end());

    // The logical comm-ring edges depend only on the job shape (not the fold
    // variant), so compute them once and reuse for OCS scoring (in the
    // selector) and the final 1-hop routes.
    const auto ring = FootprintRouter::ring_edges(job.shape);

    // Per-decision ranker context: runtime queue state (when installed)
    // enriched with current-job facts. Cleared on every exit path — the
    // ranker must never hold a pointer into this stack frame.
    SchedContext ctx = runtime_ctx_ ? *runtime_ctx_ : SchedContext{};
    ctx.current_est_duration_s =
        static_cast<double>(job.est_duration.value_or(0)) / 1e9;
    ctx.current_total_ring_edges = static_cast<int>(ring.size());
    ranker_->set_context(&ctx);
    struct CtxClear {
        PlacementRanker* r;
        ~CtxClear() {
            r->set_context(nullptr);
        }
    } ctx_clear{ranker_.get()};

    bool any_fits = false;
    std::vector<Placement> candidates;
    for (const auto& v : FoldEnumerator::enumerate(job.shape, multifold_)) {
        if (fits_cluster(v.footprint, dims)) {
            any_fits = true;
        }
        auto p = selector_->select(v, free, dims, cm, *scorer_, *ranker_, ring);
        if (p) {
            candidates.push_back(std::move(*p));
        }
    }
    const int k = ranker_->lookahead_finalists();
    if (k > 0 && candidates.size() > 1 && !ctx.queued.empty()) {
        // Pre-screen by the ranker (charges still 0), then charge the
        // finalists. Default probe: re-run enumerate+select on the
        // hypothetical free set; tests may inject ctx.probe.
        std::stable_sort(candidates.begin(), candidates.end(),
                         [&](const Placement& a, const Placement& b) {
                             return ranker_->better(a, b);
                         });
        if (!ctx.probe) {
            ctx.probe = [&](const std::array<int, 3>& shape, int nr,
                            const std::unordered_set<int>& f) {
                if (nr > static_cast<int>(f.size())) {
                    return false;
                }
                const auto rq = FootprintRouter::ring_edges(shape);
                for (const auto& vv :
                     FoldEnumerator::enumerate(shape, multifold_)) {
                    if (selector_->select(vv, f, dims, cm, *scorer_, *ranker_,
                                          rq)) {
                        return true;
                    }
                }
                return false;
            };
        }
        detail::apply_lookahead(candidates, k, lookahead_q_, gamma_, ctx, free);
    }
    std::optional<Placement> best;
    int best_idx = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (best_idx < 0 ||
            ranker_->better(candidates[i], candidates[best_idx])) {
            best_idx = i;
        }
    }
    if (best_idx >= 0) {
        best = std::move(candidates[best_idx]);
    }

    if (best) {
        r.outcome = PlacementOutcome::PLACED;
        r.npus = best->rank_map;
        r.ordered_rings = true;  // Decision D4: folded job runs rings in order
        r.reconfig_plan.ocs_edges = best->ocs_edges;
        // Pin a 1-hop route for every ring edge that is torus-adjacent or
        // wired as an OCS link. Ring edges the OCS could not realize at this
        // position (best->dor_edges of them) stay unpinned and ride the
        // backend's standard DOR.
        r.reconfig_plan.routes = FootprintRouter::adjacent_routes(
            ring, best->rank_map, dims, best->ocs_edges);
        // Owned-links invariant: every installed route is a single physical hop
        // over a link the job owns -- either a baseline torus-adjacent link, or
        // an OCS link this plan wires. Catches a future regression (e.g. a
        // non-Ring collective, or a routing change) that would otherwise route
        // silently over a non-owned / multi-hop path. (assert is active in this
        // project's builds, like the SchedRuntime deadlock assert.)
        {
            std::set<std::pair<int, int>> ocs(r.reconfig_plan.ocs_edges.begin(),
                                              r.reconfig_plan.ocs_edges.end());
            for (const auto& row : r.reconfig_plan.routes) {
                assert(row.hops.size() == 2 && row.hops.front() == row.src &&
                       row.hops.back() == row.dst &&
                       "rfold route is not a single hop");
                const int a = std::min(row.src, row.dst);
                const int b = std::max(row.src, row.dst);
                assert((cm.is_torus_adjacent(row.src, row.dst) ||
                        ocs.count({a, b}) > 0) &&
                       "rfold route hop is neither torus-adjacent nor an owned "
                       "OCS link");
            }
            for (const auto& e : r.reconfig_plan.ocs_edges) {
                assert(cm.ocs_realizable_scatter(e.first, e.second) &&
                       "rfold OCS edge is not physically realizable");
            }
            assert(cm.directions_disjoint(r.reconfig_plan.ocs_edges) &&
                   "rfold OCS edges double-book a node direction");
        }
        return r;
    }
    // 1-D fallbacks hoisted from the old standalone folding policy. A job
    // with a single communicating ring can snake through arbitrary free
    // nodes (consecutive ranks and the wrap each 1 torus hop apart), so a
    // 1-D job that fits the cluster never DROPs just because no cuboid
    // footprint fits the torus dims -- and when no fold variant fits the
    // free NPUs right now, the snake is the placement of last resort.
    const int non_unit =
        (job.shape[0] > 1) + (job.shape[1] > 1) + (job.shape[2] > 1);
    if (non_unit <= 1) {
        any_fits = true;
        auto cyc = FoldEnumerator::snake_1d(view.free_npus(), dims,
                                            job.num_ranks, kSnakeBudget);
        if (!cyc.empty()) {
            r.outcome = PlacementOutcome::PLACED;
            r.ordered_rings = true;
            // Every snake edge is torus-adjacent by construction: pin the
            // 1-hop routes; no OCS is wired.
            r.reconfig_plan.routes =
                FootprintRouter::adjacent_routes(ring, cyc, dims);
            // adjacent_routes silently skips non-1-hop edges, so check
            // completeness: a snake must pin every ring edge (both
            // directions) or it was not a 1-hop cycle.
            assert(r.reconfig_plan.routes.size() == ring.size() &&
                   "1-D snake left a ring edge unpinned (not a 1-hop cycle)");
            r.npus = std::move(cyc);
            return r;
        }
    }

    // A footprint that overflows the torus dims can still be placed by the
    // scatter path (blocks wired via OCS), so !any_fits alone cannot
    // distinguish "can never fit" from "cannot fit right now". Consult an
    // idle-cluster oracle before dropping. With permanently-failed NPUs the
    // converse gap opens too: a footprint that fits the torus dims may never
    // dodge the failed nodes, so under failures every defer consults the
    // oracle — which then answers for the degraded torus, keeping DROP
    // reserved for permanent impossibility.
    const auto& failed = view.failed_npus();
    if ((!any_fits || !failed.empty()) &&
        !placeable_on_idle(job, dims, cm, ring, failed)) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = failed.empty()
                       ? "no fold variant fits the cluster, even idle"
                       : "no fold variant fits the degraded cluster, even idle";
        return r;
    }
    r.outcome = PlacementOutcome::DEFER;
    r.reason = "no fold variant fits free NPUs right now";
    return r;
}

bool RFold::placeable_on_idle(const JobInstance& job,
                              const std::vector<int>& dims,
                              BlockModel& cm,
                              const std::vector<std::pair<int, int>>& ring,
                              const std::unordered_set<int>& failed) {
    auto it = idle_ok_.find(job.shape);
    if (it != idle_ok_.end()) {
        return it->second;
    }
    // Idle = every usable NPU free; failed ids stay excluded for the whole
    // run, so a job unplaceable here is unplaceable forever.
    std::unordered_set<int> idle_free;
    const int n = dims[0] * dims[1] * dims[2];
    idle_free.reserve(n);
    std::vector<int> idle_vec;
    idle_vec.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (failed.find(i) == failed.end()) {
            idle_free.insert(i);
            idle_vec.push_back(i);
        }
    }
    bool ok = false;
    for (const auto& v : FoldEnumerator::enumerate(job.shape, multifold_)) {
        if (selector_->select(v, idle_free, dims, cm, *scorer_, *ranker_,
                              ring)) {
            ok = true;
            break;
        }
    }
    // Mirror try_place's placement of last resort: a 1-D job whose fold
    // variants all fail can still snake through arbitrary free nodes.
    const int non_unit =
        (job.shape[0] > 1) + (job.shape[1] > 1) + (job.shape[2] > 1);
    if (!ok && non_unit <= 1) {
        ok = !FoldEnumerator::snake_1d(idle_vec, dims, job.num_ranks,
                                       kSnakeBudget)
                  .empty();
    }
    idle_ok_.emplace(job.shape, ok);
    return ok;
}

}  // namespace Scheduling
}  // namespace AstraSim
