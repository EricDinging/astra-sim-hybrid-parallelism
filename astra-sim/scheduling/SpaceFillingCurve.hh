/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_SPACEFILLINGCURVE_HH__
#define __ASTRASIM_SCHEDULING_SPACEFILLINGCURVE_HH__

#include "astra-sim/scheduling/PlacementPolicy.hh"

namespace AstraSim {
namespace Scheduling {

// 3-D Hilbert-curve placement. Orders the free NPUs by ascending Hilbert
// distance in a 2^p x 2^p x 2^p embedding cube (p = ceil(log2(max W,L,H)))
// and assigns the first N to job ranks 0..N-1 in that order. Job shape is
// consulted only for N = A*B*C; the job's internal block structure is not
// preserved in the placement. Mirrors the Python reference
// (astra-sim-artifacts/tools/placement_lib.py::SpaceFillingCurve).
class SpaceFillingCurve : public PlacementPolicy {
  public:
    PlacementResult try_place(const JobInstance& job,
                              const ClusterView& view) override;
    std::string name() const override {
        return "sfc";
    }
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
