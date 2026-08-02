/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_SEARCHSCRATCH_HH__
#define __ASTRASIM_SCHEDULING_SEARCHSCRATCH_HH__

#include "astra-sim/scheduling/BlockModel.hh"
#include "astra-sim/scheduling/BlockTiler.hh"

#include <array>
#include <cstdint>
#include <map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// Per-block free-chip census entry: free-chip count plus a bitmask of the
// free chips (x-fastest in-block bit order). Shared by every
// scatter_assign_nested call within one placement search -- the census
// depends only on (free set, block dims), both constant for the search.
struct BlockFree {
    int count = 0;
    std::vector<uint64_t> mask;
};
using ScatterCensus = std::map<std::array<int, 3>, BlockFree>;

// One tile's candidate in-block offset with its need-mask (chips the tile
// would occupy at that offset).
struct OffCand {
    std::array<int, 3> o;
    std::vector<uint64_t> need;
};

// Tiling of a footprint plus every tile's candidate offsets: a pure function
// of (footprint, block dims), so RFold memoizes it across placement searches
// (footprints recur across jobs; block dims are fixed per policy instance).
struct TilePlan {
    Tiling t;
    std::vector<std::vector<OffCand>> offs;
};

// Search-wide scratch shared across every fold variant of one placement
// search (RFold::try_place builds it once). Bundles:
//  - a flat free-membership bitmap replacing hash-set probes in the
//    contiguous scan,
//  - per-block free counts + epoch-stamped per-candidate counters that turn
//    the (frag_delta, blocks_touched) ranking keys into O(K) array walks
//    with zero hashing,
//  - the lazily-built scatter census (see BlockFree),
//  - an optional cross-search TilePlan memo owned by the policy.
// All derived values are exact reformulations of the previous per-candidate
// set/map computations: results are byte-identical by construction.
struct SearchScratch {
    SearchScratch(const std::unordered_set<int>& free,
                  const BlockModel& cm,
                  const std::vector<int>& dims)
        : free_set(&free) {
        const auto blk = cm.block_dims();
        grid = {dims[0] / blk[0], dims[1] / blk[1], dims[2] / blk[2]};
        block_vol = blk[0] * blk[1] * blk[2];
        const int nblocks = grid[0] * grid[1] * grid[2];
        free_mask.assign(static_cast<size_t>(dims[0]) * dims[1] * dims[2], 0);
        block_free.assign(nblocks, 0);
        for (int n : free) {
            free_mask[n] = 1;
            ++block_free[block_index(cm.block_of(n))];
        }
        placed_.assign(nblocks, 0);
        stamp_.assign(nblocks, 0);
        touched_.reserve(nblocks);
    }

    int block_index(const std::array<int, 3>& b) const {
        return (b[2] * grid[1] + b[1]) * grid[0] + b[0];
    }

    // (frag_delta, blocks_touched) for a candidate rank map: the number of
    // blocks taken from fully idle to partially occupied, and the number of
    // distinct blocks touched. Same values as the former set/map-based
    // count_blocks + frag_delta_of, computed in O(K) with no hashing.
    std::pair<int, int> frag_blocks(const std::vector<int>& rm,
                                    const BlockModel& cm) {
        ++epoch_;
        touched_.clear();
        for (int n : rm) {
            const int b = block_index(cm.block_of(n));
            if (stamp_[b] != epoch_) {
                stamp_[b] = epoch_;
                placed_[b] = 0;
                touched_.push_back(b);
            }
            ++placed_[b];
        }
        int frag = 0;
        for (int b : touched_) {
            // Idle before iff every chip in the block is free; fully consumed
            // after iff the placement takes all of them.
            if (block_free[b] == block_vol && placed_[b] < block_free[b]) {
                ++frag;
            }
        }
        return {frag, static_cast<int>(touched_.size())};
    }

    const std::unordered_set<int>* free_set;  // iteration (census build)
    std::vector<uint8_t> free_mask;           // O(1) membership by NPU id
    std::vector<int> block_free;              // free chips per linear block
    std::array<int, 3> grid{};                // blocks per axis
    int block_vol = 0;

    // Lazily built by the first scatter_assign_nested call of the search.
    bool census_built = false;
    ScatterCensus census;

    // Optional cross-search memo (owned by the policy; nullptr = build
    // per-call, the pre-scratch behavior).
    std::map<std::array<int, 3>, TilePlan>* tile_plan_memo = nullptr;

  private:
    std::vector<int> placed_;   // per-block placed count (epoch-stamped)
    std::vector<int> stamp_;    // epoch stamps
    std::vector<int> touched_;  // distinct blocks of the current candidate
    int epoch_ = 0;
};

}  // namespace Scheduling
}  // namespace AstraSim
#endif
