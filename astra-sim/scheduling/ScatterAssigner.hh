/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_SCATTERASSIGNER_HH__
#define __ASTRASIM_SCHEDULING_SCATTERASSIGNER_HH__

#include "astra-sim/scheduling/BlockModel.hh"
#include "astra-sim/scheduling/FoldEnumerator.hh"

#include <array>
#include <functional>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// Map a fold variant onto scattered free blocks. Returns rank_map (logical rank
// -> global NPU id) or nullopt (no realizable placement within `budget`).
// `ring` are the job's logical comm-ring edges (FootprintRouter::ring_edges),
// used to validate OCS-edge realizability. `block_rank` scores a candidate
// physical block coordinate (lower = preferred); ties break on block coord. The
// caller (selector) supplies the ranking that distinguishes min-reconfig
// (first-fit) from max-defrag (isolation-greedy).
std::optional<std::vector<int>> scatter_assign(
    const FoldVariant& v,
    const std::unordered_set<int>& free,
    const std::vector<int>& dims,
    const BlockModel& cm,
    const std::vector<std::pair<int, int>>& ring,
    const std::function<double(const std::array<int, 3>&)>& block_rank,
    int budget);

}  // namespace Scheduling
}  // namespace AstraSim
#endif
