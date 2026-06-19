/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/FailureSelection.hh"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace AstraSim {
namespace Scheduling {

std::vector<int> select_failed_npus(int npus_count,
                                    double failure_prob,
                                    unsigned seed) {
    if (npus_count <= 0) {
        return {};
    }

    double p = failure_prob;
    if (p < 0.0) {
        p = 0.0;
    } else if (p > 1.0) {
        p = 1.0;
    }

    int k = static_cast<int>(std::lround(p * npus_count));
    if (k < 0) {
        k = 0;
    } else if (k > npus_count) {
        k = npus_count;
    }

    std::vector<int> ids(npus_count);
    std::iota(ids.begin(), ids.end(), 0);

    // Partial Fisher-Yates. We deliberately do NOT use std::shuffle: its output
    // sequence is not portable across libstdc++ versions, which would break
    // reproducibility of the build-once / copy-binary-to-the-farm workflow. An
    // explicit uniform_int_distribution is reproducible for a given
    // (npus_count, p, seed).
    std::mt19937 rng(seed);
    for (int i = 0; i < k; ++i) {
        std::uniform_int_distribution<int> pick(i, npus_count - 1);
        std::swap(ids[i], ids[pick(rng)]);
    }

    std::vector<int> failed(ids.begin(), ids.begin() + k);
    std::sort(failed.begin(), failed.end());
    return failed;
}

}  // namespace Scheduling
}  // namespace AstraSim
