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
#include "astra-sim/scheduling/SpaceFillingCurve.hh"

#include <array>
#include <cstdlib>
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
          relax_on_(cfg.rfold_relax),
          relax_min_wait_ns_(static_cast<Tick>(cfg.rfold_relax_min_wait * 1e9)),
          bidi_(cfg.bidi) {}
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
    // DEFER depends only on (shape, free set) and is monotone in the free
    // set -- EXCEPT when shape relaxation is on (the outcome flips once a
    // job's wait crosses the stall floor, so it is time-dependent) or the
    // wired-probe diagnostic is active (try_place then has an observable
    // side effect at long-wait events that a skipped call would suppress).
    bool defer_is_shape_sticky() const override {
        return !relax_on_ && std::getenv("RFOLD_WIRED_PROBE") == nullptr;
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
    // Shape-constraint relaxation (--rfold-relax): when no fold variant
    // places a job that has waited >= the stall floor, fall back to the
    // lowest-link-load relaxed candidate (brick-split slabs with OCS-wired
    // seams, then SFC scatter). Need + stall floor are the whole gate.
    bool relax_on_;
    Tick relax_min_wait_ns_;
    // Bidirectional-DOR fabric (--bidi): relaxed residual edges ride the
    // shorter arc per dimension, so link-load pricing (telemetry) must use
    // min-arc hops.
    bool bidi_;
    SpaceFillingCurve relax_sfc_;
};

}  // namespace Scheduling
}  // namespace AstraSim
#endif
