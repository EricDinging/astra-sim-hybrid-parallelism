/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_FRAGMENTATIONSCORER_HH__
#define __ASTRASIM_SCHEDULING_FRAGMENTATIONSCORER_HH__

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// A candidate placement to score: the global NPU ids it would occupy plus the
// 3-D torus dims (so block tiling can decode coordinates). Lower cost is
// better.
struct ScoredPlacement {
    const std::vector<int>* npus;  // global NPU ids of the placement
    const std::vector<int>* dims;  // torus dims [Dx, Dy, Dz]
    std::array<int, 3> footprint;  // physical footprint shape (for tiebreak)
    int ocs_links = 0;             // OCS links the placement needs (rfold)
};

class FragmentationScorer {
  public:
    virtual ~FragmentationScorer() = default;
    virtual double cost(const ScoredPlacement& p) const = 0;
};

// Counts how many A×B×C blocks the placement intersects (paper heuristic).
// Tiebreak: largest footprint dimension (more cube-like = smaller). Returned as
// a single double: blocks_touched * (max_torus_dim + 1) + max_footprint_dim
// (block count dominates; tiebreak prefers more cube-like footprints).
class FewestBlocksTouched : public FragmentationScorer {
  public:
    explicit FewestBlocksTouched(std::array<int, 3> block) : block_(block) {}
    double cost(const ScoredPlacement& p) const override;

  private:
    std::array<int, 3> block_;
};

// Shape-only: largest footprint dimension.
class Compactness : public FragmentationScorer {
  public:
    double cost(const ScoredPlacement& p) const override;
};

// Lexicographic (blocks, ocs_links, max_footprint) packed into one double:
// fewest blocks dominates, then fewest OCS links, then most cube-like
// footprint. The default scorer for rfold (select with --defrag-metric
// fewest-blocks-ocs).
class FewestBlocksThenOcsLinks : public FragmentationScorer {
  public:
    explicit FewestBlocksThenOcsLinks(std::array<int, 3> block)
        : block_(block) {}
    double cost(const ScoredPlacement& p) const override;

  private:
    std::array<int, 3> block_;
};

// Parse "AxBxC" -> {A,B,C}. Returns false on malformed input.
bool parse_block_size(const std::string& s, std::array<int, 3>& out);

// name in {fewest-blocks, compactness, fewest-blocks-ocs}. block used only by
// fewest-blocks and fewest-blocks-ocs. Returns nullptr on unknown name.
std::unique_ptr<FragmentationScorer> make_fragmentation_scorer(
    const std::string& name, std::array<int, 3> block);

}  // namespace Scheduling
}  // namespace AstraSim
#endif
