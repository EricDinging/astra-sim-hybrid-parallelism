/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_CONTIGUOUSSCAN_HH__
#define __ASTRASIM_SCHEDULING_CONTIGUOUSSCAN_HH__

#include "astra-sim/scheduling/FoldEnumerator.hh"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// For fold variant `v`, sweep the 6 axis-rotations and wrap-around anchors over
// torus `dims`. For the FIRST all-free placement at each (rotation, ay, az)
// anchor, invoke on_fit(rank_map, physical_footprint). Sets any_fits_cluster if
// some rotation's footprint fits the cluster dims (regardless of occupancy) --
// the caller uses it to distinguish DEFER from DROP. Callers score/keep-best.
// `free_mask` is a flat by-NPU-id membership bitmap (SearchScratch::free_mask):
// the probe loop runs up to 6 x N anchors x K chips per variant, so membership
// must be an array index, not a hash probe.
void scan_contiguous_fits(
    const FoldVariant& v,
    const std::vector<int>& dims,
    const std::uint8_t* free_mask,
    int K,
    bool& any_fits_cluster,
    const std::function<void(const std::vector<int>&,
                             const std::array<int, 3>&)>& on_fit);

}  // namespace Scheduling
}  // namespace AstraSim
#endif
