/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/ContiguousScan.hh"

#include <algorithm>

namespace AstraSim {
namespace Scheduling {

void scan_contiguous_fits(
    const FoldVariant& v,
    const std::vector<int>& dims,
    const std::unordered_set<int>& free,
    int K,
    bool& any_fits_cluster,
    const std::function<void(const std::vector<int>&,
                             const std::array<int, 3>&)>& on_fit) {
    std::array<int, 3> ax{0, 1, 2};
    do {
        std::array<int, 3> pf{0, 0, 0};  // footprint mapped onto cluster axes
        for (int j = 0; j < 3; ++j) {
            pf[ax[j]] = v.footprint[j];
        }
        if (pf[0] > dims[0] || pf[1] > dims[1] || pf[2] > dims[2]) {
            continue;
        }
        any_fits_cluster = true;
        auto upper = [&](int i) { return pf[i] == dims[i] ? 1 : dims[i]; };
        for (int az = 0; az < upper(2); ++az) {
            for (int ay = 0; ay < upper(1); ++ay) {
                for (int axx = 0; axx < upper(0); ++axx) {
                    std::array<int, 3> anchor{axx, ay, az};
                    std::vector<int> cand(K);
                    bool ok = true;
                    for (int i = 0; i < K && ok; ++i) {
                        const auto& e = v.embedding[i];
                        int cc[3];
                        for (int j = 0; j < 3; ++j) {
                            cc[ax[j]] = (anchor[ax[j]] + e[j]) % dims[ax[j]];
                        }
                        int n =
                            cc[2] * dims[1] * dims[0] + cc[1] * dims[0] + cc[0];
                        if (free.find(n) == free.end()) {
                            ok = false;
                        } else {
                            cand[i] = n;
                        }
                    }
                    if (ok) {
                        on_fit(cand, pf);
                        break;  // first fit at this (ay,az); continue ay/az
                    }
                }
            }
        }
    } while (std::next_permutation(ax.begin(), ax.end()));
}

}  // namespace Scheduling
}  // namespace AstraSim
