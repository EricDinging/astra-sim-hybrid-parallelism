/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/BlockTiler.hh"

#include <algorithm>

namespace AstraSim {
namespace Scheduling {

Tiling tile(const std::array<int, 3>& footprint, std::array<int, 3> block) {
    Tiling t{};  // value-initialize grid_dims/offset_free
    for (int i = 0; i < 3; ++i) {
        t.grid_dims[i] = (footprint[i] + block[i] - 1) / block[i];  // ceil
        t.offset_free[i] = footprint[i] < block[i];
    }
    for (int gz = 0; gz < t.grid_dims[2]; ++gz) {
        for (int gy = 0; gy < t.grid_dims[1]; ++gy) {
            for (int gx = 0; gx < t.grid_dims[0]; ++gx) {
                Block b;
                b.grid = {gx, gy, gz};
                for (int i = 0; i < 3; ++i) {
                    b.fp_offset[i] = b.grid[i] * block[i];
                    int rem = footprint[i] - b.fp_offset[i];
                    b.shape[i] = std::min(block[i], rem);
                }
                t.blocks.push_back(b);
            }
        }
    }
    return t;
}

}  // namespace Scheduling
}  // namespace AstraSim
