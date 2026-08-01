/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __WORKLOAD_HH__
#define __WORKLOAD_HH__

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "astra-sim/system/Callable.hh"
#include "astra-sim/system/CommunicatorGroup.hh"
#include "astra-sim/workload/HardwareResource.hh"
#include "astra-sim/workload/LocalMemUsageTracker.hh"
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
    std::unordered_map<uint64_t, uint64_t> collective_comm_node_id_map;
    std::unordered_map<uint64_t, DataSet*> collective_comm_wrapper_map;
    bool is_finished;

    int job_local_rank = -1;
    Scheduling::JobInstance* parent_job = nullptr;

    // node id -> comm group id for every grouped COMM_COLL node, built by the
    // constructor's Kahn scan and consumed by HardwareResource::comm_group_of
    // (avoids a feeder attribute fetch per occupancy check). Node ids are
    // iteration-invariant, so the map survives per-iteration feeder rebuilds.
    std::unordered_map<uint64_t, int> node_cg_map_;

    // Stream-id layout for scheduled jobs:
    //   ((job_id % kJobIdWindow) * kMaxCGPerJob + cg_id) * kStreamsPerCG
    //     + ordinal * kMaxStreamsPerCollective + intra-collective offset.
    // kJobIdWindow * kMaxCGPerJob * kStreamsPerCG must exactly tile the
    // collective tag window (see Sys::FrontEndSendRecvType and the
    // static_assert in the Workload constructor), so a stream id can never
    // leave [0, COLLECTIVE) -- the tag mapping in front_end_sim_send stays a
    // no-op and ids stay non-negative at any job count. Jobs whose ids are
    // kJobIdWindow apart reuse a tag range; that is always safe, independent of
    // kJobIdWindow's value, because (a) concurrent jobs never share NPUs
    // (exclusive placement) so their (src,dst) pairs never overlap even under a
    // shared base, and (b) a job's NPUs are returned to the free pool only
    // after every rank has fully drained (SchedRuntime::detach_job runs only
    // once JobInstance::on_rank_finished sees all ranks finished, and a rank
    // finishes only when Workload::current_iteration_drained() reports
    // num_in_flight_gpu_comm_ops == 0 -- i.e. all collective chunks delivered).
    // So a range-reuse twin can never collide with an in-flight chunk of its
    // predecessor. kJobIdWindow is thus a divisor knob for the tiling, not a
    // correctness margin, and can be as small as the tiling requires.
    static constexpr int kMaxStreamsPerCollective = 64;
    // kMaxCGPerJob caps comm groups per job; the whole-torus job is the worst
    // case (a 16x16x16/4096-NPU job emits 4608 comm groups; an 8x8x8/512-NPU
    // job emits 640). kStreamsPerCG / kMaxStreamsPerCollective caps collectives
    // per comm group per iteration (measured max 162 across both the cluster512
    // and cluster4096 shape sets, at the concentrated-TP/no-pipeline shapes).
    // The three factors multiply to exactly the COLLECTIVE tag-window width
    // (static_assert in the constructor), which is 2^8 * 5^9 = 500,000,000, so
    // every factor must be a 2^a*5^b divisor and raising one forces lowering
    // another. 8 * 5000 * 12500 is the exact tiling that clears both floors
    // (cg cap 5000 >= 4609; collectives/cg ceiling 12500/64 = 195 >= 162) with
    // the most kJobIdWindow the window allows.
    static constexpr int kMaxCGPerJob = 5000;
    static constexpr int kStreamsPerCG = 12500;
    static constexpr int kJobIdWindow = 8;

  private:
    // Legacy one-shot path only (feeds a constructor debug log).
    std::vector<int> comm_group_list;

    // From the ET node, find out the corresponding communicator group, and
    // return the pointer. If no communicator group is specified for this ET
    // node, return nullptr.
    CommunicatorGroup* extract_comm_group(
        std::shared_ptr<Chakra::ETFeederNode> node);

    // (cg_id, node_id) -> ordinal map, pre-computed from the trace via a
    // min-node-id Kahn traversal, so ordinals are (a) invariant across ranks
    // of a CG even if ranks issue collectives in different orders and (b) a
    // linear extension of the trace DAG (required by the in-order admission
    // gate below; plain node-id order is not dependency-consistent).
    std::unordered_map<int, std::unordered_map<uint64_t, int>>
        cg_node_to_ordinal_;

    // --- Deadlock-free collective admission ---------------------------------
    // A comm group's collectives are admitted strictly in ordinal order (the
    // rank-invariant order from cg_node_to_ordinal_), at most
    // HardwareResource::kMaxInflightCommPerGroup in flight per group. Because
    // every member rank admits the same collectives in the same order, the
    // oldest unfinished collective of a group always gets a slot on all of its
    // ranks, so circular waits between collectives cannot form (the failure
    // mode behind the SchedRuntime liveness valve).
    //
    // cg_unadmitted_ordinals_ holds, per comm group, the ordinals not yet
    // admitted in the CURRENT iteration; only the smallest one is admissible.
    // A plain cursor would wedge after a valve bypass admits out of order, so
    // we track the exact unadmitted set instead. Rebuilt (from
    // cg_node_to_ordinal_) at construction and at every iteration boundary.
    std::unordered_map<int, std::set<int>> cg_unadmitted_ordinals_;

    // --- Ordered mirror of the resolver's dependency-free set --------------
    // issue_dep_free_nodes() must iterate free nodes in ascending id order
    // (deterministic, rank-invariant issue). The resolver's truth is an
    // unordered_set, and copying + sorting it on every completion event is
    // O(F log F) with F large whenever ranks are cap-blocked. Instead keep an
    // ordered mirror, updated at every point Workload mutates the resolver
    // (take/push_back/finish/reset -- all resolver mutations for this feeder
    // live in Workload.cc; CustomAlgorithm owns a separate feeder). Every
    // issue_dep_free_nodes() entry asserts an O(1) size tripwire against the
    // truth (asserts are live even in this project's release build); the
    // sanitizer (Debug) build additionally checks element-wise equality.
    std::set<uint64_t> dep_free_mirror_;
    // Reusable snapshot buffer for issue_dep_free_nodes passes; keeps its
    // capacity across calls to avoid per-event reallocation.
    std::vector<uint64_t> dep_free_snapshot_;
    // finish_node + mirror maintenance: snapshots the node's children before
    // finish_node consumes the edges, then mirrors the children it freed.
    void dep_finish_node(uint64_t node_id);
    // Re-derive the mirror from the resolver's truth (constructor / iteration
    // rewind).
    void rebuild_dep_free_mirror();

    // True iff admitting `node` now respects the per-group admission order
    // (always true for non-collectives, ungrouped collectives, and while the
    // liveness-valve bypass is active).
    bool comm_admission_in_order(
        std::shared_ptr<Chakra::FeederV3::ETFeederNode> node) const;
    // Record a successful admission: drop the node's ordinal from its group's
    // unadmitted set. No-op for nodes without a tracked ordinal.
    void mark_comm_admitted(
        std::shared_ptr<Chakra::FeederV3::ETFeederNode> node);
    // Reset per-group admission state to "nothing admitted yet"; called at
    // construction and at each iteration boundary (node ids repeat across
    // iterations).
    void reset_comm_admission_order();

    // --- Multi-iteration (training-loop) support ---------------------------
    // The single-iteration Chakra trace is replayed total_iterations_ times to
    // model a job that runs N training iterations. Each iteration is a full
    // drain of the DAG; iteration k+1 only begins once iteration k has fully
    // completed, which gives an exact per-iteration barrier for free. The
    // trace path is retained so the feeder can be rebuilt (rewound) per
    // iteration. Legacy one-shot path leaves total_iterations_ at 1.
    std::string workload_filename_;
    int total_iterations_ = 1;
    int current_iteration_ = 0;

    // True once the current iteration's DAG has fully drained: no dep-free or
    // ongoing nodes in the resolver and no in-flight hardware ops.
    bool current_iteration_drained() const;
    // Rewind to a fresh single-iteration DAG (new feeder => fresh dependency
    // resolver) and reset per-iteration trackers. Reuses every logical id
    // (comm-group ids, deterministic stream-id bases, tags); resets no global
    // counter. See the definition for the safety argument.
    void advance_to_next_iteration();
    // Terminal completion: report, notify the frontend, flip is_finished and
    // signal the parent job. Called once, after the final iteration drains.
    void finish();
};

}  // namespace AstraSim

#endif /* __WORKLOAD_HH__ */
