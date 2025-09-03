/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/CommonNetworkApi.hh"
#include <astra-network-analytical/reconfigurable/Topology.h>
#include <astra-network-analytical/reconfigurable/TopologyManager.h>

using namespace AstraSim;
using namespace AstraSimAnalytical;
using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

namespace AstraSimAnalyticalReconfigurable {

class ReconfigurableNetworkApi final : public CommonNetworkApi {
  public:
    /**
     * Set the topology to be used.
     *
     * @param tm_ptr pointer to the topology
     */
    static void set_topology(std::shared_ptr<TopologyManager> tm_ptr) noexcept;

    /**
     * Constructor.
     *
     * @param rank id of the API
     */
    explicit ReconfigurableNetworkApi(int rank) noexcept;

    /**
     * Implement sim_send of AstraNetworkAPI.
     */
    int sim_send(void* buffer,
                 uint64_t count,
                 int type,
                 int dst,
                 int tag,
                 sim_request* request,
                 void (*msg_handler)(void* fun_arg),
                 void* fun_arg) override;

  void sim_reconfig(int topo_id) override;

  void increment_inflight_coll() override;

  void decrement_inflight_coll() override;

  private:
    /// topology
    static std::shared_ptr<TopologyManager> tm;
};

}  // namespace AstraSimAnalyticalReconfigurable
