/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ROOT_FETCH_RING_HH__
#define __ROOT_FETCH_RING_HH__

#include "astra-sim/system/astraccl/native_collectives/collective_algorithm/Ring.hh"

namespace AstraSim {

class RootFetchRing : public Ring {
  public:
    enum class QuarterPlacement { Q1, Q2, Q3 };

    RootFetchRing(ComType type,
                  int id,
                  RingTopology* ring_topology,
                  uint64_t data_size,
                  RingTopology::Direction direction,
                  InjectionPolicy injection_policy);

  private:
    int compute_extra_fetch_messages(RingTopology* ring_topology) const;
    int failure_round_for(int failure_index) const;
};

/*
 * Hard-coded experiment knobs for the root-fetch surrogate.
 *
 * Supported values:
 *   g_root_fetch_mf       in {1, 2, 4}
 *   g_root_fetch_quarter  in {Q1, Q2, Q3}
 *
 * Update these globals directly before rebuilding if you want a different
 * scenario.
 */
extern int g_root_fetch_mf;
extern RootFetchRing::QuarterPlacement g_root_fetch_quarter;

}  // namespace AstraSim

#endif /* __ROOT_FETCH_RING_HH__ */
