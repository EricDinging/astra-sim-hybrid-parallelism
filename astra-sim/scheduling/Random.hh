/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_RANDOM_HH__
#define __ASTRASIM_SCHEDULING_RANDOM_HH__

#include "astra-sim/scheduling/PlacementPolicy.hh"

#include <random>

namespace AstraSim {
namespace Scheduling {

class Random : public PlacementPolicy {
  public:
    Random();
    PlacementResult try_place(const JobInstance& job,
                              const ClusterView& view) override;
    std::string name() const override {
        return "random";
    }

  private:
    std::mt19937 rng_;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
