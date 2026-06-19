/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_TOPOMATCH_HH__
#define __ASTRASIM_SCHEDULING_TOPOMATCH_HH__

#include "astra-sim/scheduling/PlacementPolicy.hh"
#include "astra-sim/scheduling/TopoMatchSolver.hh"

#include <memory>

namespace AstraSim {
namespace Scheduling {

// TopoMatch placement on a 3-D torus. Builds the job's K x K affinity matrix
// from its Chakra traces and asks libtopomatch to map ranks onto free NPUs,
// minimizing communication cost. Requires 3-D physical_dims (--npus-per-dim).
class TopoMatch : public PlacementPolicy {
  public:
    PlacementResult try_place(const JobInstance& job,
                              const ClusterView& view) override;
    std::string name() const override {
        return "topomatch";
    }

  private:
    std::unique_ptr<TopoMatchSolver> solver_;  // lazily built once dims known
    int sw_ = -1, sl_ = -1, sh_ = -1;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
