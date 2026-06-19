/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_FREENPUS_HH__
#define __ASTRASIM_SCHEDULING_FREENPUS_HH__

#include <unordered_set>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// Returns the ascending list of NPU ids in [0, total_npus) that are neither
// busy nor failed. Single tested home for the cluster-view exclusion logic.
inline std::vector<int> free_npus_excluding(
    int total_npus,
    const std::unordered_set<int>& busy,
    const std::unordered_set<int>& failed) {
    std::vector<int> free;
    free.reserve(static_cast<std::size_t>(total_npus));
    for (int i = 0; i < total_npus; ++i) {
        if (busy.find(i) == busy.end() && failed.find(i) == failed.end()) {
            free.push_back(i);
        }
    }
    return free;
}

}  // namespace Scheduling
}  // namespace AstraSim

#endif
