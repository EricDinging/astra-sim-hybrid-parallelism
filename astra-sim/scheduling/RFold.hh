/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_RFOLD_HH__
#define __ASTRASIM_SCHEDULING_RFOLD_HH__

#include "astra-sim/scheduling/BlockSelector.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/FragmentationScorer.hh"
#include "astra-sim/scheduling/PlacementPolicy.hh"
#include "astra-sim/scheduling/PlacementRanker.hh"
#include "astra-sim/scheduling/SchedContext.hh"

#include <array>
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// RFold: fold-aware placement with OCS reconfiguration. Reuses FoldEnumerator
// + a pluggable BlockSelector and PlacementRanker; emits a ReconfigPlan
// (owned OCS edges + 1-hop routes) so a folded job's collective rings run on
// real 1-hop links. With --block-size = the whole torus there is a single
// block and the only realizable OCS pairs are full-axis wraps that already
// exist as torus links, so no OCS edge is ever wired and the policy
// degenerates to pure folding (the former standalone "folding" policy).
// ordered_rings=true. The BlockModel is built per-call from the cluster dims
// + block dims.
class RFold : public PlacementPolicy {
  public:
    RFold(std::array<int, 3> block,
          std::unique_ptr<BlockSelector> selector,
          std::unique_ptr<FragmentationScorer> scorer,
          std::unique_ptr<PlacementRanker> ranker,
          const PlacementConfig& cfg = PlacementConfig{})
        : block_(block),
          selector_(std::move(selector)),
          scorer_(std::move(scorer)),
          ranker_(std::move(ranker)),
          multifold_(cfg.rfold_multifold),
          lookahead_q_(cfg.cm_lookahead_q),
          gamma_(cfg.cm_gamma) {}
    PlacementResult try_place(const JobInstance& job,
                              const ClusterView& view) override;
    std::string name() const override {
        return "rfold";
    }
    void set_sched_context(const SchedContext* ctx) override {
        runtime_ctx_ = ctx;
    }
    bool wants_sched_context() const override {
        return ranker_->wants_context();
    }

  private:
    // True iff `shape` places on a fully-idle cluster: some fold variant
    // selects, or (1-D shapes) the snake closes. "Idle" means every usable
    // NPU free — `failed` ids are excluded, so under permanent NPU failures
    // the oracle answers for the degraded torus, not the pristine one.
    // Decides DROP vs DEFER whenever fit-now checks cannot tell "can never
    // fit" from "cannot fit right now": footprint-overflow shapes that only
    // the scatter path places (e.g. 32x2x1 on 8x8x8), and any shape once
    // failures exist (a fitting footprint may never dodge the failed nodes).
    // Memoized per shape; sound because the failed set is selected once at
    // startup and never changes during a run.
    bool placeable_on_idle(const JobInstance& job,
                           const std::vector<int>& dims,
                           BlockModel& cm,
                           const std::vector<std::pair<int, int>>& ring,
                           const std::unordered_set<int>& failed);

    std::array<int, 3> block_;
    std::unique_ptr<BlockSelector> selector_;
    std::unique_ptr<FragmentationScorer> scorer_;
    std::unique_ptr<PlacementRanker> ranker_;
    std::map<std::array<int, 3>, bool> idle_ok_;
    const SchedContext* runtime_ctx_ = nullptr;
    bool multifold_;
    // Consumed by the cost-model lookahead pass in try_place.
    int lookahead_q_;
    double gamma_;
};

namespace detail {
// Lookahead charging pass (cost-model): truncate `cands` (already ranker-
// sorted, best first) to the top k finalists, then charge each finalist
// gamma * est_duration_s for every queued job (FCFS order, top q) it flips
// from placeable-now to blocked. ctx.probe must be set. Exposed for tests.
void apply_lookahead(std::vector<Placement>& cands,
                     int k,
                     int q,
                     double gamma,
                     const SchedContext& ctx,
                     const std::unordered_set<int>& free);
}  // namespace detail

}  // namespace Scheduling
}  // namespace AstraSim
#endif
