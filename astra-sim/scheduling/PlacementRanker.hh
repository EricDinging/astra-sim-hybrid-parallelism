/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_PLACEMENTRANKER_HH__
#define __ASTRASIM_SCHEDULING_PLACEMENTRANKER_HH__

#include "astra-sim/scheduling/BlockSelector.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/SchedContext.hh"

#include <memory>
#include <string>

namespace AstraSim {
namespace Scheduling {

// Pluggable placement preference for RFold (--rfold-ranking). Rankers turn
// the facts a Placement carries (identity / ring_closes / scattered,
// blocks_touched, dor_edges, pure scorer cost) into a strict weak order; the
// selectors and RFold's cross-variant loop both rank through this interface.
class PlacementRanker {
  public:
    virtual ~PlacementRanker() = default;
    // True iff a is strictly better than b.
    virtual bool better(const Placement& a, const Placement& b) const = 0;
    // State hook for context-aware rankers (cost-model, switch). The pointer
    // is valid only until the next call; callers re-install per decision and
    // clear with nullptr afterwards. Fixed rankers ignore it.
    virtual void set_context(const SchedContext* ctx) {
        (void)ctx;
    }
    // True iff set_context is actually consumed. Lets SchedRuntime skip
    // building the sched context (a copy + sort of the whole pending queue)
    // for the fixed rankers, which never read it.
    virtual bool wants_context() const {
        return false;
    }
    // When false, MinReconfig's "fully-closed contiguous wins immediately"
    // early-return is skipped so scatter can compete in the ranker
    // (cost-model with the scatter guard off). Everything else returns true.
    virtual bool scatter_never_beats_closed() const {
        return true;
    }
    // Top-K finalists the policy should probe in the lookahead pass before
    // the final rank; 0 = no lookahead. Only cost-model overrides.
    virtual int lookahead_finalists() const {
        return 0;
    }
};

// Comm-class ranking (spec 2026-06-11 §3). Hard class order
//   0 closed:    contiguous, every ring edge 1-hop (dor_edges == 0)
//   1 open:      contiguous, some edges ride DOR
//   2 scattered: strictly last — scatter only when nothing contiguous
//                of any fold variant fits
// Within-class key (classes 0/1):
//   (dor_edges, blocks_touched[, ocs_links][, residual_frag], cost)
// The optional keys are the rfoldv1/rfoldv2 split (both on for v1, both off
// for v2):
//  - residual_frag ("wall-hugging"): root-caused 2026-07-18 as the
//    blocksize-valley fragmenter (steers placements toward enclosed pockets,
//    shredding the free space big jobs need).
//  - ocs_links (circuit frugality): dropped 2026-07-21 under the RDCN model
//    revision — every inter-block seam is an optical circuit (the default
//    torus is just the boot configuration), so "prefer default seams" prices
//    an adjacency that does not exist on the real machine.
// v1 keeps both for bit-compatibility with pre-2026-07 sweeps. (The 2026-06
// stage-2 ablation arms — ocs-first, legacy — were deleted with their study.)
class CommFirst : public PlacementRanker {
  public:
    explicit CommFirst(bool v1_keys = true) : v1_keys_(v1_keys) {}
    bool better(const Placement& a, const Placement& b) const override;

  private:
    bool v1_keys_;
};

// The pre-unification rfold ranking, bit-compatible: (blocks_touched,
// dor_edges, cost + 0.5 if open contiguous variant). Packing is primary
// because throughput under load is governed by how tightly jobs pack -- a
// placement that strands free capacity makes later jobs queue, while
// dor-edge hop counts only affect one job's collective latency. The +0.5
// ring-closing tiebreak applies only to contiguous candidates, exactly like
// the ContiguousFirst hack it replaces (scatter placements always carried
// raw scorer cost).
// Precondition (holds since the ranker wiring): Placement::cost is the raw
// scorer cost; the legacy in-selector +0.5 was deleted from ContiguousFirst.
class PackingFirst : public PlacementRanker {
  public:
    bool better(const Placement& a, const Placement& b) const override;
};

// State-aware scalar ranking (spec 2026-06-11 §4): cost in seconds,
//   cost(p) = C_comm + C_ext + C_rcfg
//   C_comm = kappa * dor_edges * est_dur_s / total_ring_edges
//            (kappa ABSORBS the spec's comm_fraction(job) — the roofline
//            estimator exposes no compute/comm split — so kappa is an
//            empirically-calibrated constant, not a pure unit factor)
//   C_ext  = EITHER the proxy term (when cm_lookahead_k == 0):
//            beta * residual_frag/(6*blocks), with beta = c_ext * Q *
//            mean(t_svc) (dynamic) or the frozen cm_beta_const_s constant
//            (the "cost-model-static" ablation arm)
//            OR lookahead_ext_s (when probing is on, cm_lookahead_k > 0) --
//            the lookahead arm REPLACES the proxy so the sweep arms compare
//            cleanly; they are alternative C_ext implementations, never summed
//   C_rcfg = t_reconfig_s * ocs_links
// Interpolates the fixed rankers: Q=0 behaves comm-first-like, deep
// backlog behaves packing-first-like. Scatter-last stays a HARD guard
// unless cm_scatter_guard is off (the externality term polices it then).
class CostModelRanker : public PlacementRanker {
  public:
    explicit CostModelRanker(const PlacementConfig& cfg)
        : kappa_(cfg.cm_kappa),
          c_ext_(cfg.cm_c_ext),
          t_reconfig_s_(cfg.cm_t_reconfig_s),
          beta_const_s_(cfg.cm_beta_const_s),
          guard_(cfg.cm_scatter_guard),
          lookahead_k_(cfg.cm_lookahead_k) {}
    bool better(const Placement& a, const Placement& b) const override;
    void set_context(const SchedContext* ctx) override {
        ctx_ = ctx;
    }
    bool wants_context() const override {
        return true;
    }
    bool scatter_never_beats_closed() const override {
        return guard_;
    }
    int lookahead_finalists() const override {
        return lookahead_k_;
    }
    double cost_of(const Placement& p) const;  // exposed for tests

  private:
    double kappa_, c_ext_, t_reconfig_s_, beta_const_s_;
    bool guard_;
    int lookahead_k_;
    const SchedContext* ctx_ = nullptr;
};

// Evaluation baseline: comm-first when the queue is shallow, packing-first
// when backlogged. Exists to show whether the cost model earns its
// complexity over naive regime switching. Deliberately discontinuous: the
// ranking flips whole-hog at depth == theta (vs the cost model's smooth
// interpolation), so theta=1 toggles every time the queue crosses 1 —
// regime thrash is part of what the baseline measures.
class DynamicSwitch : public PlacementRanker {
  public:
    explicit DynamicSwitch(int theta) : theta_(theta) {}
    bool better(const Placement& a, const Placement& b) const override {
        const bool backlogged = ctx_ != nullptr && ctx_->queue_depth >= theta_;
        return backlogged ? pack_.better(a, b) : comm_.better(a, b);
    }
    void set_context(const SchedContext* ctx) override {
        ctx_ = ctx;
    }
    bool wants_context() const override {
        return true;
    }

  private:
    CommFirst comm_;
    PackingFirst pack_;
    int theta_;
    const SchedContext* ctx_ = nullptr;
};

// name in {comm-first, comm-first-no-residual, packing-first, cost-model,
// switch}. Returns nullptr on unknown name.
std::unique_ptr<PlacementRanker> make_placement_ranker(
    const std::string& name, const PlacementConfig& cfg = PlacementConfig{});

}  // namespace Scheduling
}  // namespace AstraSim
#endif
