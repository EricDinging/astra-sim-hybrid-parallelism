/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

// TODO: HardwareResource.hh should be moved to the system layer.

#ifndef __HARDWARE_RESOURCE_HH__
#define __HARDWARE_RESOURCE_HH__

#include "astra-sim/common/Logging.hh"
#include <cstdint>
#include <unordered_map>

#include "extern/graph_frontend/chakra/src/feeder_v3/et_feeder.h"

namespace AstraSim {

class HardwareResource {
  public:
    HardwareResource(uint32_t num_npus, int sys_id = -1);
    ~HardwareResource() {
        auto logger = LoggerFactory::get_logger("HardwareResource");
        if (this->num_in_flight_cpu_ops != 0 ||
            this->num_in_flight_gpu_comm_ops != 0 ||
            this->num_in_flight_gpu_comp_ops != 0) {
            logger->critical(
                "!!!Hardware Resource sys.id={} has unreleased nodes!!!",
                this->sys_id);
        }
        for (auto node_id : cpu_ops_node) {
            logger->critical("CPU node id: {}", node_id);
        }
        for (auto node_id : gpu_ops_node) {
            logger->critical("GPU comp node id: {}", node_id);
        }
        for (auto node_id : gpu_comms_node) {
            logger->critical("GPU comm node id: {}", node_id);
        }
    }
    // Cap on in-flight UNGROUPED comm ops (sends and any collective without a
    // pg_name). Sends complete without the receiver holding a slot (RECV nodes
    // never occupy one), so this cap cannot form a cross-rank wait cycle.
    static constexpr uint32_t kMaxInflightGpuCommOps = 2;

    // Per-comm-group window for GROUPED collectives: at most this many
    // in-flight collectives per communicator group. Workload additionally
    // admits a group's collectives strictly in a rank-invariant order (see
    // Workload::comm_admission_in_order), which together with the per-group
    // window makes collective admission deadlock-free: the oldest unfinished
    // collective of a group is always admissible on every member rank.
    static constexpr uint32_t kMaxInflightCommPerGroup = 2;

    // Liveness valve (backstop): when the global event queue drains while jobs
    // are incomplete, SchedRuntime sets this flag, re-issues dep-free nodes
    // ignoring the caps and the admission order, and clears it. With per-group
    // in-order admission this should never fire; if it does, it signals a
    // trace whose dependency structure contradicts the node-id admission order
    // (or an ungrouped-collective cycle on the legacy path).
    bool comm_cap_bypass = false;

    void occupy(const std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    void release(const std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    bool is_available(
        const std::shared_ptr<Chakra::FeederV3::ETFeederNode> node) const;
    void report();

    // Communicator-group id of a collective node (its integer pg_name), or -1
    // for non-collectives and collectives without a parseable pg_name.
    static int comm_group_of(
        const std::shared_ptr<Chakra::FeederV3::ETFeederNode>& node);

    std::unordered_set<uint64_t> cpu_ops_node;
    std::unordered_set<uint64_t> gpu_ops_node;
    std::unordered_set<uint64_t> gpu_comms_node;

    const int sys_id;

    const uint32_t num_npus;
    uint32_t num_in_flight_cpu_ops;
    uint32_t num_in_flight_gpu_comp_ops;
    // Total in-flight comm ops (grouped + ungrouped). Not a cap by itself;
    // consulted by Workload::current_iteration_drained and the destructor
    // sanity check.
    uint32_t num_in_flight_gpu_comm_ops;
    // In-flight comm ops without a comm group (sends, pg-less collectives),
    // capped at kMaxInflightGpuCommOps.
    uint32_t num_in_flight_ungrouped_comm_ops;
    // In-flight collectives per comm group, capped at kMaxInflightCommPerGroup.
    std::unordered_map<int, uint32_t> num_in_flight_comm_per_group;

    uint64_t num_cpu_ops;
    uint64_t num_gpu_ops;
    uint64_t num_gpu_comms;

    uint64_t tics_cpu_ops;
    uint64_t tics_gpu_ops;
    uint64_t tics_gpu_comms;
};

}  // namespace AstraSim

#endif /* __HARDWARE_RESOURCE_HH__ */
