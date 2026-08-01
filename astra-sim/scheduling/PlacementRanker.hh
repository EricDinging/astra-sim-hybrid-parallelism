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
    // State hook for context-aware rankers (switch). The pointer is valid
    // only until the next call; callers re-install per decision and clear
    // with nullptr afterwards. Fixed rankers ignore it.
    virtual void set_context(const SchedContext* ctx) {
        (void)ctx;
    }
    // True iff set_context is actually consumed. Lets SchedRuntime skip
    // building the sched context for the fixed rankers, which never read it.
    virtual bool wants_context() const {
        return false;
    }
    // When false, MinReconfig's "fully-closed contiguous wins immediately"
    // early-return is skipped so scatter can compete in the ranker.
    // Everything else returns true.
    virtual bool scatter_never_beats_closed() const {
        return true;
    }
    // Lazy-ranking hook: true iff `incumbent` strictly beats EVERY possible
    // candidate whose (frag_delta, blocks_touched) equal the given values,
    // regardless of the candidate's remaining keys. Selectors then skip the
    // expensive per-candidate construction (OCS realizability, wirable
    // subset, scorer cost) for candidates that already lost. Only rankers
    // whose order is lexicographic with exactly this prefix can say true;
    // the default (false) means "always build and compare fully".
    virtual bool prefix_beats(const Placement& incumbent,
                              int frag_delta,
                              int blocks_touched) const {
        (void)incumbent;
        (void)frag_delta;
        (void)blocks_touched;
        return false;
    }
};

// Comm-class ranking (spec 2026-06-11 §3). Hard class order
//   0 closed:    contiguous, every ring edge 1-hop (dor_edges == 0)
//   1 open:      contiguous, some edges ride DOR
//   2 scattered: strictly last — scatter only when nothing contiguous
//                of any fold variant fits
// Within-class key (classes 0/1): (dor_edges, blocks_touched, cost).
// Scatter class (2) leads with frag_delta — newly-fragmented blocks — then
// (blocks_touched, dor_edges, cost).
class CommFirst : public PlacementRanker {
  public:
    bool better(const Placement& a, const Placement& b) const override;
};

// The default ranking (2026-07-22): fragmentation-first, no contiguity
// tier. Every placement, solid box or tiled, ranks by (frag_delta,
// blocks_touched, dor_edges, cost): fewest newly-fragmented cubes, then
// fewest cubes, then communication, then the scorer tiebreak. Under the
// RDCN model there is no proximity, so contiguous-vs-non-contiguous is not
// a first-principles distinction; with tile nesting keeping non-contiguous
// placements frugal, the tier measurably only cost performance (5k-job
// probes, mean JCT: fifo-0.60 -4.8%, fifo-0.90 -15%, easy-0.90 -2.5% from
// dropping it).
class FragFirst : public PlacementRanker {
  public:
    bool better(const Placement& a, const Placement& b) const override;
    bool scatter_never_beats_closed() const override {
        return false;  // tiled placements compete as equals, always searched
    }
    // The order is strictly lexicographic (frag_delta, blocks_touched,
    // dor_edges, cost), so an incumbent strictly ahead on the first two keys
    // beats any candidate with this prefix no matter its dor_edges/cost.
    bool prefix_beats(const Placement& incumbent,
                      int frag_delta,
                      int blocks_touched) const override {
        return incumbent.frag_delta < frag_delta ||
               (incumbent.frag_delta == frag_delta &&
                incumbent.blocks_touched < blocks_touched);
    }
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

// name in {comm-first, frag-first, packing-first, switch}. Returns nullptr
// on unknown name.
std::unique_ptr<PlacementRanker> make_placement_ranker(
    const std::string& name, const PlacementConfig& cfg = PlacementConfig{});

}  // namespace Scheduling
}  // namespace AstraSim
#endif
