/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_BLOCKTILER_HH__
#define __ASTRASIM_SCHEDULING_BLOCKTILER_HH__

#include <array>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// One tiled block of a footprint. A block embeds into a contiguous sub-region
// of ONE physical block.
struct Block {
    std::array<int, 3> grid;       // block-grid coordinate
    std::array<int, 3> fp_offset;  // origin in footprint-local coords
    std::array<int, 3> shape;      // extent (<= block[axis] per axis)
};

// The result of tiling a footprint into blocks.
struct Tiling {
    std::vector<Block> blocks;     // x-fastest grid order
    std::array<int, 3> grid_dims;  // # blocks per axis
    // axis a has a free within-block offset DOF iff footprint[a] < block[a]
    // (a single sub-block segment that can slide within a block -> block
    // sharing).
    std::array<bool, 3> offset_free;
};

// Split `footprint` along each axis into segments of length `block[axis]` (last
// may be a partial slab). Deterministic, side-effect free.
Tiling tile(const std::array<int, 3>& footprint, std::array<int, 3> block);

}  // namespace Scheduling
}  // namespace AstraSim
#endif
