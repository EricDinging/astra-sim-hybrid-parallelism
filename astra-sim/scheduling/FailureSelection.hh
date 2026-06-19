/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_FAILURESELECTION_HH__
#define __ASTRASIM_SCHEDULING_FAILURESELECTION_HH__

#include <vector>

namespace AstraSim {
namespace Scheduling {

// Selects exactly round(failure_prob * npus_count) distinct NPU ids to mark as
// failed, drawn uniformly from [0, npus_count) using a reproducible partial
// Fisher-Yates shuffle seeded by `seed`. `failure_prob` is clamped to [0, 1].
// Returns the failed ids in ascending order (empty when the rounded count is
// 0).
std::vector<int> select_failed_npus(int npus_count,
                                    double failure_prob,
                                    unsigned seed);

}  // namespace Scheduling
}  // namespace AstraSim

#endif
