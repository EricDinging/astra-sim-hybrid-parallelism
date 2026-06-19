/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __WORKLOAD_HH__
#define __WORKLOAD_HH__

#include <memory>
#include <string>
#include <unordered_map>

#include "astra-sim/system/Callable.hh"
#include "astra-sim/system/CommunicatorGroup.hh"
#include "astra-sim/workload/HardwareResource.hh"
#include "astra-sim/workload/LocalMemUsageTracker.hh"
#include "astra-sim/workload/Scheduler.hh"
#include "astra-sim/workload/Statistics.hh"
#include "extern/graph_frontend/chakra/src/feeder_v3/et_feeder.h"

namespace AstraSim {
namespace Scheduling {
class JobInstance;
}
}  // namespace AstraSim

namespace AstraSim {

class Sys;
class DataSet;
class Scheduler;

class Workload : public Callable {
  public:
    // DEPRECATED: do NOT use in new code. Constructs a Workload bound to a
    // single per-NPU trace file (the legacy one-shot path). Kept only because
    // the legacy Sys constructor (used by congestion_aware, congestion_unaware,
    // htsim, and ns3 frontends) still calls it. The supported frontend is the
    // reconfigurable analytical backend; new code should use the
    // (Sys*, Scheduling::JobInstance*, int) constructor below.
    Workload(Sys* sys,
             std::string et_filename,
             std::string comm_group_filename);

    // Dynamic-scheduling constructor: opens
    // <parent_job->trace_dir>/chakra_trace.<job_local_rank>.et and translates
    // any job-local ranks (in comm_group.json or in send/recv nodes) through
    // parent_job->rank_map to global NPU ids at issue time.
    Workload(Sys* sys, Scheduling::JobInstance* parent_job, int job_local_rank);

    ~Workload();

    // communicator groups
    // Parse the user provided 'comm_group_filename' and extract the list of
    // communicator groups. Refer to the wiki for the format.
    void initialize_comm_groups(std::string comm_group_filename);
    void issue_pytorch_pg_metadata(
        std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);

    // event-based simulation
    void issue_dep_free_nodes();
    bool issue(std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    void issue_metadata(std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    void issue_replay(std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    void issue_remote_mem(std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    void issue_comp(std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    bool issue_comm(std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    bool issue_coll_comm(std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    bool issue_send_comm(std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    bool issue_recv_comm(std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    void skip_invalid(std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    void call(EventType event, CallData* data);
    void fire();

    // stats
    void report();

    Chakra::ETFeeder* et_feeder;
    std::unordered_map<int, CommunicatorGroup*> comm_groups;
    HardwareResource* hw_resource;
    Sys* sys;
    Statistics* stats;
    std::unique_ptr<LocalMemUsageTracker> local_mem_usage_tracker;
    std::unordered_map<int, uint64_t> collective_comm_node_id_map;
    std::unordered_map<int, DataSet*> collective_comm_wrapper_map;
    bool is_finished;
    Scheduler* scheduler;

    int job_local_rank = -1;
    Scheduling::JobInstance* parent_job = nullptr;

    static constexpr int kMaxStreamsPerCollective = 64;

  private:
    // From the ET node, find out the corresponding communicator group, and
    // return the pointer. If no communicator group is specified for this ET
    // node, return nullptr.
    std::vector<int> comm_group_list;
    int current_comm_group_idx;

    int cached_reconfig_topo_id;

    CommunicatorGroup* extract_comm_group(
        std::shared_ptr<Chakra::ETFeederNode> node);
    int previous_group_id = 0;

    // (cg_id, node_id) -> ordinal map, pre-computed from the trace so
    // ordinals are invariant across ranks of a CG even if ranks issue
    // collectives in different orders.
    std::unordered_map<int, std::unordered_map<uint64_t, int>>
        cg_node_to_ordinal_;
};

}  // namespace AstraSim

#endif /* __WORKLOAD_HH__ */
