/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __SYSTEM_HH__
#define __SYSTEM_HH__

#include <chrono>
#include <unordered_map>

#include "astra-sim/common/AstraNetworkAPI.hh"
#include "astra-sim/common/AstraRemoteMemoryAPI.hh"
#include "astra-sim/network_frontend/analytical/include/reconfigurable/ReconfigurableNetworkApi.hh"
#include "astra-sim/system/Callable.hh"
#include "astra-sim/system/CollectivePhase.hh"
#include "astra-sim/system/CommunicatorGroup.hh"
#include "astra-sim/system/MemBus.hh"
#include "astra-sim/system/Roofline.hh"
#include "astra-sim/system/astraccl/native_collectives/logical_topology/RingTopology.hh"
#include "astra-sim/workload/Workload.hh"

namespace AstraSim {

class BaseStream;
class StreamBaseline;
class DataSet;
class QueueLevels;
class Workload;
class LogicalTopology;
class BasicLogicalTopology;
class OfflineGreedy;

class Sys : public Callable {
  public:
    // SchedulerUnit
    // ------------------------------------------------------------
    class SchedulerUnit {
      public:
        SchedulerUnit(Sys* sys,
                      std::vector<int> queues,
                      int max_running_streams,
                      int ready_list_threshold,
                      int queue_threshold);
        void notify_stream_added(int vnet);
        void notify_stream_added_into_ready_list();
        void notify_stream_removed(int vnet, Tick running_time);
        std::vector<double> get_average_latency_per_dimension();

        Sys* sys;
        int max_running_streams;
        int ready_list_threshold;
        int queue_threshold;
        // Indexed by dense queue (vnet) id 0..total_queues-1; sized once at
        // construction (queue ids are contiguous by design).
        std::vector<int> running_streams;
        std::vector<std::list<BaseStream*>::iterator> stream_pointer;
        std::vector<Tick> latency_per_dimension;
        std::vector<double> total_chunks_per_dimension;
        std::vector<int> queue_id_to_dimension;
    };
    //---------------------------------------------------------------------------

    // Constructor / Destructor
    // -------------------------------------------------
    // DEPRECATED: do NOT use in new code. This constructor builds a Sys with
    // a single immutable Workload bound at construction time (the legacy
    // one-shot multi-tenant flow). It is kept only because the
    // congestion_aware, congestion_unaware, htsim, and ns3 frontends have not
    // been migrated to the dynamic-scheduling runtime. The supported frontend
    // for new work is the reconfigurable analytical backend, which uses the
    // workload-less constructor below.
    Sys(int id,
        std::string workload_configuration,
        std::string comm_group_configuration,
        std::string system_configuration,
        AstraRemoteMemoryAPI* remote_mem,
        AstraNetworkAPI* comm_NI,
        std::vector<int> physical_dims,
        std::vector<int> queues_per_dim,
        double injection_scale,
        bool rendezvous_enabled);

    // Workload-less constructor for the dynamic-scheduling runtime
    // (reconfigurable analytical backend only). SchedRuntime attaches a
    // Workload per job via attach_workload() at placement time and detaches
    // it on completion.
    Sys(int id,
        std::string system_configuration,
        AstraRemoteMemoryAPI* remote_mem,
        AstraNetworkAPI* comm_NI,
        std::vector<int> physical_dims,
        std::vector<int> queues_per_dim,
        double injection_scale,
        bool rendezvous_enabled);

    ~Sys();
    //---------------------------------------------------------------------------

    // Intialization
    // ------------------------------------------------------------
    bool initialize_sys(std::string name);
    CollectiveImpl* generate_collective_impl_from_input(
        std::string collective_impl_str);
    CollectiveImpl* generate_custom_collective_impl(
        std::string collective_impl_str);
    //---------------------------------------------------------------------------

    // Helper Functions
    // ---------------------------------------------------------
    static Tick boostedTick();
    static void sys_panic(std::string msg);
    //---------------------------------------------------------------------------

    // Simulation Loop
    // ----------------------------------------------------------
    void exit_sim_loop(std::string msg);
    //---------------------------------------------------------------------------

    // General Event Handling
    // ---------------------------------------------------
    void call(EventType type, CallData* data);
    void call_events();
    void register_event(Callable* callable,
                        EventType event,
                        CallData* callData,
                        Tick delta_cycles);
    void try_register_event(Callable* callable,
                            EventType event,
                            CallData* callData,
                            Tick& delta_cycles);
    static void handleEvent(void* arg);
    //---------------------------------------------------------------------------

    // Communicator Group Support
    // -----------------------------------------------
    LogicalTopology* get_logical_topology(ComType comm_type);
    std::vector<CollectiveImpl*> get_collective_implementation(
        ComType comm_type);
    //---------------------------------------------------------------------------

    // Collective Communication Primitives
    // --------------------------------------
    DataSet* generate_all_reduce(uint64_t size,
                                 const std::vector<bool>& involved_dimensions,
                                 CommunicatorGroup* communicator_group,
                                 int explicit_priority);
    DataSet* generate_all_to_all(uint64_t size,
                                 const std::vector<bool>& involved_dimensions,
                                 CommunicatorGroup* communicator_group,
                                 int explicit_priority);
    DataSet* generate_all_gather(uint64_t size,
                                 const std::vector<bool>& involved_dimensions,
                                 CommunicatorGroup* communicator_group,
                                 int explicit_priority);
    DataSet* generate_reduce_scatter(
        uint64_t size,
        const std::vector<bool>& involved_dimensions,
        CommunicatorGroup* communicator_group,
        int explicit_priority);
    DataSet* generate_collective(
        uint64_t size,
        LogicalTopology* topology,
        const std::vector<CollectiveImpl*>& implementation_per_dimension,
        const std::vector<bool>& dimensions_involved,
        ComType collective_type,
        int explicit_priority,
        CommunicatorGroup* communicator_group);
    CollectivePhase generate_collective_phase(ComType collective_type,
                                              BasicLogicalTopology* topology,
                                              uint64_t data_size,
                                              int queue_id,
                                              RingTopology::Direction direction,
                                              InjectionPolicy injection_policy,
                                              CollectiveImpl* collective_impl);
    int break_dimension(int model_parallel_npu_group);
    //---------------------------------------------------------------------------

    // Middle-level Network Primitives
    // ------------------------------------------
    uint64_t determine_chunk_size(uint64_t& size, ComType type);
    int get_priority(int explicit_priority);
    void insert_into_ready_list(BaseStream* stream);
    void insert_stream(std::list<BaseStream*>* queue, BaseStream* baseStream);
    void schedule(int num);
    void proceed_to_next_vnet_baseline(StreamBaseline* stream);
    //---------------------------------------------------------------------------

    // Low-level Network Primitives
    // ---------------------------------------------
    enum FrontEndSendRecvType {
        // NATIVE means send/recv is issued directly by workload input
        // COLLECTIVE means send/recv is caused by a collective communication
        // RENDEZVOUS means send/recv is a rendezvous shake hand
        // The value here presents the offset of the tag. The tag range for
        // different type is as follows
        // NATIVE: [0, 500000000)
        // COLLECTIVE: [500000000, 1000000000)
        // RENDEZVOUS: [1000000000, 2000000000)
        NATIVE = 0,
        COLLECTIVE = 500000000,
        RENDEZVOUS = 1000000000
    };
    int front_end_sim_send(Tick delay,
                           void* buffer,
                           uint64_t count,
                           int type,
                           int dst,
                           int tag,
                           sim_request* request,
                           FrontEndSendRecvType send_type,
                           void (*msg_handler)(void* fun_arg),
                           void* fun_arg);

    int front_end_sim_recv(Tick delay,
                           void* buffer,
                           uint64_t count,
                           int type,
                           int src,
                           int tag,
                           sim_request* request,
                           FrontEndSendRecvType recv_type,
                           void (*msg_handler)(void* fun_arg),
                           void* fun_arg);

    int rendezvous_sim_send(Tick delay,
                            void* buffer,
                            uint64_t count,
                            int type,
                            int dst,
                            int tag,
                            sim_request* request,
                            void (*msg_handler)(void* fun_arg),
                            void* fun_arg);

    int rendezvous_sim_recv(Tick delay,
                            void* buffer,
                            uint64_t count,
                            int type,
                            int src,
                            int tag,
                            sim_request* request,
                            void (*msg_handler)(void* fun_arg),
                            void* fun_arg);

    int sim_send(Tick delay,
                 void* buffer,
                 uint64_t count,
                 int type,
                 int dst,
                 int tag,
                 sim_request* request,
                 void (*msg_handler)(void* fun_arg),
                 void* fun_arg);

    bool sim_reconfig(int topo_id);

    void increment_inflight_coll();
    void decrement_inflight_coll();
    int get_inflight_coll();

    int sim_recv(Tick delay,
                 void* buffer,
                 uint64_t count,
                 int type,
                 int src,
                 int tag,
                 sim_request* request,
                 void (*msg_handler)(void* fun_arg),
                 void* fun_arg);
    //---------------------------------------------------------------------------

    static std::vector<Sys*> all_sys;  // vector of all Sys objects

    int id;
    bool initialized;

    // workload
    Workload* workload;
    Workload* active_workload = nullptr;  // non-owning, nullable
    void attach_workload(Workload* w) {
        active_workload = w;
    }
    void detach_workload() {
        active_workload = nullptr;
        num_streams = 0;
        total_running_streams = 0;
        first_phase_streams = 0;
        // Per-NPU scheduler state must not bleed to the next tenant (P4-12).
        // Under the current config this is a no-op: LIFO priorities are only
        // compared among live streams (none at detach), and the round-robin/
        // offline-greedy inter-dimension schedulers are unused. But a
        // RoundRobin/OfflineGreedy run would otherwise make a job's dimension
        // ordering depend on which jobs previously ran on its NPUs.
        round_robin_inter_dimension_scheduler = 0;
        priority_counter = 0;
        last_scheduled_collective = 0;
    }

    // roofline model
    bool roofline_enabled;
    double peak_perf;
    Roofline* roofline;

    // memory
    bool track_local_mem;
    std::string local_mem_trace_filename;
    double local_mem_bw;
    AstraRemoteMemoryAPI* remote_mem;

    // memory bus
    MemBus* memBus;
    float inp_L;
    float inp_o;
    float inp_g;
    float inp_G;
    bool model_shared_bus;
    double injection_scale;
    int communication_delay;
    int local_reduction_delay;

    // network
    AstraNetworkAPI* comm_NI;
    bool rendezvous_enabled;

    // scheduler
    SchedulerUnit* scheduler_unit;
    QueueLevels* vLevels;
    OfflineGreedy* offline_greedy;
    IntraDimensionScheduling intra_dimension_scheduling;
    InterDimensionScheduling inter_dimension_scheduling;
    int round_robin_inter_dimension_scheduler;
    int active_chunks_per_dimension;
    int priority_counter;
    uint64_t pending_events;
    int preferred_dataset_splits;
    int concurrent_streams;
    int active_first_phase;
    int max_running;

    // for supporting LIFO
    std::list<BaseStream*> ready_list;
    SchedulingPolicy scheduling_policy;
    int first_phase_streams;
    int total_running_streams;
    std::map<int, std::list<BaseStream*>> active_Streams;

    // Keyed bucket store, never iterated in time order (the network backend
    // owns the time-ordered queue), so a hash map avoids the red-black tree's
    // O(log n) lookups and per-node allocation. Buckets are vectors (no
    // per-entry node allocation), iterated BY INDEX in call_events(). NOTE:
    // callbacks invoked from call_events() may register further events, which
    // can rehash this map and reallocate the current bucket -- so never hold
    // an iterator across a callback; bind a reference to the bucket vector
    // (mapped references are stable across rehash), copy each tuple out
    // before invoking, and erase by key.
    std::unordered_map<Tick,
                       std::vector<std::tuple<Callable*, EventType, CallData*>>>
        event_queue;
    int total_nodes;
    int dim_to_break;
    std::vector<int> logical_broken_dims;

    std::vector<int> physical_dims;
    std::vector<int> queues_per_dim;

    // collective communication
    int num_streams;
    static uint8_t* dummy_data;
    std::map<std::string, LogicalTopology*> logical_topologies;
    // Cached raw pointers to the four logical_topologies entries, so the
    // per-collective path skips the string-keyed map probe. Assigned at
    // every site that (re)populates the map (both constructors and
    // break_dimension); the map remains the owner.
    LogicalTopology* all_reduce_topology = nullptr;
    LogicalTopology* reduce_scatter_topology = nullptr;
    LogicalTopology* all_gather_topology = nullptr;
    LogicalTopology* all_to_all_topology = nullptr;
    std::vector<CollectiveImpl*> all_reduce_implementation_per_dimension;
    std::vector<CollectiveImpl*> reduce_scatter_implementation_per_dimension;
    std::vector<CollectiveImpl*> all_gather_implementation_per_dimension;
    std::vector<CollectiveImpl*> all_to_all_implementation_per_dimension;
    CollectiveOptimization collectiveOptimization;
    Tick last_scheduled_collective;
    bool break_dimension_done;
    int dimension_to_break;

    // statistics
    bool trace_enabled;

    // skip simulation for all nodes and use current duration
    bool replay_only;
};

}  // namespace AstraSim

#endif /* __SYSTEM_HH__ */
