/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_FIRSTFIT_HH__
#define __ASTRASIM_SCHEDULING_FIRSTFIT_HH__

#include "astra-sim/scheduling/PlacementPolicy.hh"

#include <array>
#include <map>

namespace AstraSim {
namespace Scheduling {

class FirstFit : public PlacementPolicy {
  public:
    PlacementResult try_place(const JobInstance& job,
                              const ClusterView& view) override;
    std::string name() const override {
        return "firstfit";
    }

  private:
    // True iff `job` places on the cluster with every usable NPU free.
    // Decides DROP vs DEFER under permanent NPU failures: scattered failed
    // nodes can make every anchor x rotation of a cuboid unsatisfiable
    // forever, which the geometric fit check cannot see. Memoized per shape;
    // sound because the failed set is selected once at startup and never
    // changes during a run.
    bool placeable_on_idle(const JobInstance& job, const ClusterView& view);

    std::map<std::array<int, 3>, bool> idle_ok_;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
