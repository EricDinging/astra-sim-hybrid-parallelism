/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_BLOCKSELECTOR_HH__
#define __ASTRASIM_SCHEDULING_BLOCKSELECTOR_HH__

#include "astra-sim/scheduling/BlockModel.hh"
#include "astra-sim/scheduling/FoldEnumerator.hh"
#include "astra-sim/scheduling/FragmentationScorer.hh"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// A chosen placement for one fold variant.
struct Placement {
    std::vector<int> rank_map;                   // rank -> global NPU id
    std::array<int, 3> footprint;                // physical footprint (scoring)
    std::vector<std::pair<int, int>> ocs_edges;  // global cross-block OCS edges
    double cost;
    // Distinct A×B×C blocks the placement intersects: the packing signal used
    // by PlacementRanker. (cost's leading term is also block count, but cost
    // bundles an anti-OCS ocs_links penalty below it, so cost cannot serve as
    // the packing key without suppressing useful OCS wiring -- hence this
    // explicit field.)
    int blocks_touched = 0;
    // Ring-edge pairs that are neither torus-adjacent nor OCS-realizable at
    // this position: they ride the backend's standard DOR (unpinned)
    // instead of a pinned 1-hop route. Used as a secondary tiebreak by
    // PlacementRanker.
    int dor_edges = 0;
    // Sum over the touched blocks of free_neighbor_blocks against the
    // PRE-placement free set: lower = the placement consumes already-isolated
    // blocks (wall-hugging). Final fixed-ranking tiebreak before cost.
    int residual_frag = 0;
    // Queue-externality charge filled by RFold's cost-model lookahead pass;
    // 0 otherwise. Read only by CostModelRanker.
    double lookahead_ext_s = 0.0;
    int ocs_links() const {
        return static_cast<int>(ocs_edges.size());
    }
    // Construction facts consumed by PlacementRanker keys (set by whichever
    // selector builds the placement; rankers derive comm classes from these
    // rather than the selector baking preference into cost):
    bool identity = false;     // FoldVariant::identity (the unfolded variant)
    bool ring_closes = false;  // FoldVariant::ring_closes
    bool scattered = false;    // produced by scatter_assign (non-contiguous)
};

class PlacementRanker;

// Pluggable strategy that maps a fold variant onto real blocks/NPUs.
class BlockSelector {
  public:
    virtual ~BlockSelector() = default;
    // `ranker` orders candidate placements (within this selector's scan and
    // across variants in RFold); `ring_edges` are the job's logical comm-ring
    // edges as rank pairs (from FootprintRouter::ring_edges(shape));
    // variant-independent, so the caller computes them once and passes them
    // in for OCS-edge scoring.
    virtual std::optional<Placement> select(
        const FoldVariant& v,
        const std::unordered_set<int>& free,
        const std::vector<int>& dims,
        const BlockModel& cm,
        const FragmentationScorer& scorer,
        const PlacementRanker& ranker,
        const std::vector<std::pair<int, int>>& ring_edges) const = 0;
    virtual std::string name() const = 0;
};

// Contiguous placement (folding's anchor-scan); OCS realizes the ring edges
// whose endpoints aren't torus-adjacent (closures / cross-block). Reused by
// min-reconfig's contiguous phase.
class ContiguousFirst : public BlockSelector {
  public:
    std::optional<Placement> select(
        const FoldVariant& v,
        const std::unordered_set<int>& free,
        const std::vector<int>& dims,
        const BlockModel& cm,
        const FragmentationScorer& scorer,
        const PlacementRanker& ranker,
        const std::vector<std::pair<int, int>>& ring_edges) const override;
    std::string name() const override {
        return "contiguous";
    }
};

// Scatter selector: place contiguously when possible (reusing ContiguousFirst,
// so it is a superset of contiguous placeability), otherwise scatter onto the
// first-fit free blocks (fewest reconfigured OCS links).
class MinReconfig : public BlockSelector {
  public:
    explicit MinReconfig(int search_budget) : budget_(search_budget) {}
    std::optional<Placement> select(
        const FoldVariant& v,
        const std::unordered_set<int>& free,
        const std::vector<int>& dims,
        const BlockModel& cm,
        const FragmentationScorer& scorer,
        const PlacementRanker& ranker,
        const std::vector<std::pair<int, int>>& ring_edges) const override;
    std::string name() const override {
        return "min-reconfig";
    }

  private:
    int budget_;
};

// Scatter selector: gather the most-isolated free blocks (fewest free
// neighbor-blocks) to preserve large contiguous holes for future jobs.
class MaxDefrag : public BlockSelector {
  public:
    explicit MaxDefrag(int search_budget) : budget_(search_budget) {}
    std::optional<Placement> select(
        const FoldVariant& v,
        const std::unordered_set<int>& free,
        const std::vector<int>& dims,
        const BlockModel& cm,
        const FragmentationScorer& scorer,
        const PlacementRanker& ranker,
        const std::vector<std::pair<int, int>>& ring_edges) const override;
    std::string name() const override {
        return "max-defrag";
    }

  private:
    int budget_;
};

// name in {contiguous, min-reconfig, max-defrag}. search_budget bounds the
// scatter selectors' backtracking. Returns nullptr on unknown name.
std::unique_ptr<BlockSelector> make_block_selector(const std::string& name,
                                                   int search_budget = 50000);

}  // namespace Scheduling
}  // namespace AstraSim
#endif
