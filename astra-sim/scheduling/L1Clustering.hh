/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_L1CLUSTERING_HH__
#define __ASTRASIM_SCHEDULING_L1CLUSTERING_HH__

#include "astra-sim/scheduling/PlacementPolicy.hh"

namespace AstraSim {
namespace Scheduling {

// L1 Clustering placement on a 3-D torus. Samples candidate center NPUs
// from the free pool, finds the k closest free NPUs (by torus-wrapped
// Manhattan distance) for each center, and picks the center that minimizes
// the total distance. Job shape is consulted only for k = A*B*C; the
// job's internal block structure is not preserved. Mirrors the Python
// reference (astra-sim-artifacts/tools/placement_lib.py::L1Clustering).
class L1Clustering : public PlacementPolicy {
  public:
    PlacementResult try_place(const JobInstance& job,
                              const ClusterView& view) override;
    std::string name() const override {
        return "l1clustering";
    }
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
