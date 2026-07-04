/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/workload/Workload.hh"

#include "astra-sim/common/Logging.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/system/IntData.hh"
#include "astra-sim/system/MemEventHandlerData.hh"
#include "astra-sim/system/RecvPacketEventHandlerData.hh"
#include "astra-sim/system/SendPacketEventHandlerData.hh"
#include "astra-sim/system/WorkloadLayerHandlerData.hh"
#include <json/json.hpp>

#include <algorithm>
#include <iostream>
#include <queue>
#include <stdlib.h>
#include <unistd.h>

using namespace std;
using namespace AstraSim;
using namespace Chakra::FeederV3;
using json = nlohmann::json;

typedef ChakraProtoMsg::NodeType ChakraNodeType;
typedef ChakraProtoMsg::CollectiveCommType ChakraCollectiveCommType;

Workload::Workload(Sys* sys, string et_filename, string comm_group_filename) {
    string workload_filename = et_filename + "." + to_string(sys->id) + ".et";
    // Check if workload filename exists
    if (access(workload_filename.c_str(), R_OK) < 0) {
        string error_msg;
        if (errno == ENOENT) {
            error_msg =
                "workload file: " + workload_filename + " does not exist";
        } else if (errno == EACCES) {
            error_msg = "workload file: " + workload_filename +
                        " exists but is not readable";
        } else {
            error_msg =
                "Unknown workload file: " + workload_filename + " access error";
        }
        LoggerFactory::get_logger("workload")->critical(error_msg);
        exit(EXIT_FAILURE);
    }
    auto temp_et_feeder = new ETFeeder(workload_filename);
    this->comm_group_list = temp_et_feeder->traverse_comm_group();
    delete temp_et_feeder;
    this->et_feeder = new ETFeeder(workload_filename);
    this->workload_filename_ = workload_filename;
    // Legacy one-shot path: always a single iteration (total_iterations_ stays
    // at its default of 1).
    this->comm_groups.clear();
    // TODO: parametrize the number of available hardware resources
    this->hw_resource = new HardwareResource(1, sys->id);
    this->local_mem_usage_tracker =
        std::make_unique<LocalMemUsageTracker>(sys->id);
    this->sys = sys;
    initialize_comm_groups(comm_group_filename);
    this->stats = new Statistics(this);
    this->is_finished = false;

    auto logger = LoggerFactory::get_logger("workload");
    std::string comm_group_str;
    for (int comm_group_id : this->comm_group_list) {
        comm_group_str += std::to_string(comm_group_id) + " ";
    }
    logger->debug("Workload::Workload, sys->id={}. Comm groups: {}", sys->id,
                  comm_group_str);
}

Workload::Workload(Sys* sys,
                   Scheduling::JobInstance* parent_job,
                   int job_local_rank) {
    this->sys = sys;
    this->parent_job = parent_job;
    this->job_local_rank = job_local_rank;

    std::string workload_filename = parent_job->trace_dir + "/chakra_trace." +
                                    std::to_string(job_local_rank) + ".et";
    if (access(workload_filename.c_str(), R_OK) < 0) {
        std::string error_msg =
            "workload file: " + workload_filename + " not readable";
        LoggerFactory::get_logger("workload")->critical(error_msg);
        std::exit(EXIT_FAILURE);
    }
    // (No comm_group_list scan here: it fed only a legacy-path debug log,
    // and the scan cost a full extra trace parse per rank per attach.)

    // Pre-compute (cg_id, node_id) -> ordinal map. Two consumers, two
    // requirements:
    //   * the deterministic stream-ID computation in issue_coll_comm() needs
    //     ordinals that are invariant across all ranks of a CG;
    //   * per-group in-order collective admission (comm_admission_in_order)
    //     additionally needs the ordinal order to be a linear extension of
    //     the trace DAG. Plain node-id order is NOT one: the trace generator
    //     numbers nodes per layer at definition time, so a layer's backward
    //     collective can transitively depend on higher-id nodes, and
    //     admitting in id order wedges TP groups.
    // A Kahn traversal that always visits the smallest dependency-free node
    // id satisfies both: deterministic, identical on every member rank of a
    // CG (member ranks carry identical DAGs), and dependency-consistent.
    {
        auto* scan_feeder = new ETFeeder(workload_filename);
        auto& resolver = scan_feeder->getDependancyResolver();
        const auto& dep_layer = resolver.get_enabled_dependancy();
        std::priority_queue<uint64_t, std::vector<uint64_t>,
                            std::greater<uint64_t>>
            ready;
        std::unordered_set<uint64_t> queued;
        for (const uint64_t node_id : resolver.get_dependancy_free_nodes()) {
            ready.push(node_id);
            queued.insert(node_id);
        }
        std::unordered_map<int, std::vector<uint64_t>> cg_to_node_ids;
        // Rank-invariance fingerprint (see check below): FNV-1a over the
        // ordinal-ordered (name, comm_size) of each CG's collectives.
        // Logical identity -- not node ids -- is what tag pairing needs: the
        // k-th admitted collective of a CG must be the same collective on
        // every member rank.
        constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
        constexpr uint64_t kFnvPrime = 1099511628211ULL;
        std::unordered_map<int, uint64_t> cg_fingerprint;
        const auto fnv_fold = [](uint64_t h, const void* data,
                                 size_t len) noexcept {
            const auto* p = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < len; ++i) {
                h = (h ^ p[i]) * kFnvPrime;
            }
            return h;
        };
        while (!ready.empty()) {
            const uint64_t node_id = ready.top();
            ready.pop();
            auto node = scan_feeder->lookupNode(node_id);
            if (node->type() == ChakraNodeType::COMM_COLL_NODE) {
                std::string pg = node->pg_name<std::string>("");
                if (!pg.empty()) {
                    try {
                        int cg_id = std::stoi(pg);
                        cg_to_node_ids[cg_id].push_back(node_id);
                        node_cg_map_[node_id] = cg_id;
                        auto it =
                            cg_fingerprint.emplace(cg_id, kFnvOffset).first;
                        const std::string name = node->name();
                        const uint64_t comm_size = node->comm_size();
                        uint64_t h =
                            fnv_fold(it->second, name.data(), name.size());
                        h = fnv_fold(h, &comm_size, sizeof(comm_size));
                        it->second = h;
                    } catch (const std::exception&) {
                    }
                }
            }
            // Snapshot children before finish_node mutates the layer, then
            // enqueue those that became dependency-free.
            const auto children = dep_layer.get_children(node_id);
            resolver.take_node(node_id);
            resolver.finish_node(node_id);
            const auto& now_free = resolver.get_dependancy_free_nodes();
            for (const uint64_t child : children) {
                if (now_free.count(child) != 0U &&
                    queued.insert(child).second) {
                    ready.push(child);
                }
            }
        }
        delete scan_feeder;
        for (auto& kv : cg_to_node_ids) {
            auto& ord_map = cg_node_to_ordinal_[kv.first];
            for (size_t i = 0; i < kv.second.size(); ++i) {
                ord_map[kv.second[i]] = static_cast<int>(i);
            }
        }
        // Deadlock-freedom of per-group ordered admission (and correctness
        // of ordinal-derived stream ids/tags) requires every member rank of
        // a CG to derive the same ordinal sequence of collectives. That
        // holds when member ranks carry structurally identical DAGs -- true
        // for the STG generator, but nothing enforced it. Verify at attach:
        // the first member rank records each CG's fingerprint in the
        // JobInstance; every later member must match, else the job would
        // wedge at runtime with no indication why.
        for (const auto& [cg_id, fp] : cg_fingerprint) {
            auto ins = parent_job->cg_ordinal_fingerprint.emplace(cg_id, fp);
            if (!ins.second && ins.first->second != fp) {
                LoggerFactory::get_logger("workload")
                    ->critical("job {} rank {}: comm group {} ordinal sequence "
                               "diverges "
                               "from earlier member ranks (fingerprint {:#x} "
                               "vs {:#x}); "
                               "per-group ordered admission requires identical "
                               "collective order on every member rank",
                               parent_job->job_id, job_local_rank, cg_id, fp,
                               ins.first->second);
                std::exit(1);
            }
        }
        reset_comm_admission_order();
    }

    this->et_feeder = new ETFeeder(workload_filename);
    // Snapshot the sealed dependency graph so advance_to_next_iteration can
    // rewind the resolver in memory instead of re-parsing the trace per
    // iteration. Must happen before any node is issued.
    this->et_feeder->capture_pristine_dependancy();
    this->workload_filename_ = workload_filename;
    // Replay the single-iteration trace once per training iteration. Clamp to
    // >= 1 so a malformed/zero count degrades to single-iteration rather than
    // never running.
    this->total_iterations_ = std::max(1, parent_job->num_iterations);
    this->comm_groups.clear();
    this->hw_resource = new HardwareResource(1, sys->id);
    // Node ids are iteration-invariant, so the map stays valid across the
    // per-iteration resolver rewinds.
    this->hw_resource->set_node_cg_map(&node_cg_map_);
    this->local_mem_usage_tracker =
        std::make_unique<LocalMemUsageTracker>(sys->id);

    // Per-job comm_group.json (optional).
    std::string cg_path = parent_job->trace_dir + "/comm_group.json";
    if (access(cg_path.c_str(), R_OK) < 0) {
        LoggerFactory::get_logger("workload")
            ->info("no comm_group.json for job {}; relying on inline pg "
                   "metadata or no collectives",
                   parent_job->job_id);
    } else {
        std::ifstream inFile(cg_path);
        if (!inFile.is_open()) {
            // access() passed but open failed: almost always fd exhaustion
            // (EMFILE), which otherwise surfaces as an opaque nlohmann
            // parse_error ("unexpected end of input").
            LoggerFactory::get_logger("workload")
                ->critical("job {}: cannot open {} (errno={}); out of file "
                           "descriptors?",
                           parent_job->job_id, cg_path, errno);
            std::exit(EXIT_FAILURE);
        }
        json j;
        try {
            inFile >> j;
        } catch (const std::exception& e) {
            LoggerFactory::get_logger("workload")
                ->critical("job {}: cannot parse {}: {}", parent_job->job_id,
                           cg_path, e.what());
            std::exit(EXIT_FAILURE);
        }
        for (json::iterator it = j.begin(); it != j.end(); ++it) {
            int cg_id = 0;
            try {
                cg_id = std::stoi(it.key());
            } catch (const std::exception&) {
                LoggerFactory::get_logger("workload")
                    ->critical("job {}: non-numeric comm group key '{}' in {}",
                               parent_job->job_id, it.key(), cg_path);
                std::exit(EXIT_FAILURE);
            }
            std::vector<int> translated;
            for (const auto& local_rank_json : it.value()) {
                int local_rank = local_rank_json.get<int>();
                if (local_rank < 0 || local_rank >= parent_job->num_ranks) {
                    LoggerFactory::get_logger("workload")
                        ->critical("job {}: comm_group.json rank {} out of "
                                   "range",
                                   parent_job->job_id, local_rank);
                    std::exit(1);
                }
                translated.push_back(parent_job->rank_map[local_rank]);
            }
            if (cg_id < 0 || cg_id >= kMaxCGPerJob) {
                LoggerFactory::get_logger("workload")
                    ->critical("job {}: comm group id {} outside [0, {}) in "
                               "{} -- unsupported by the stream-id layout",
                               parent_job->job_id, cg_id, kMaxCGPerJob,
                               cg_path);
                std::exit(1);
            }
            auto* cg = new CommunicatorGroup(cg_id, translated, sys,
                                             parent_job->ordered_rings);
            // Stream IDs derive from CommunicatorGroup::num_streams (set to
            // id * 1000000 by default). Since all jobs share the same local
            // cg_ids, concurrent jobs would collide in the network
            // frontend's tag space. Override with a (job_id mod window,
            // cg_id)-unique base so every comm group of every concurrently
            // running job gets its own range. The layout exactly tiles the
            // collective tag window, so ids can never overflow int, go
            // negative, or escape the window (see Workload.hh for the
            // window-reuse safety argument).
            static_assert(static_cast<int64_t>(kJobIdWindow) * kMaxCGPerJob *
                                  kStreamsPerCG ==
                              static_cast<int64_t>(
                                  Sys::FrontEndSendRecvType::COLLECTIVE) -
                                  Sys::FrontEndSendRecvType::NATIVE,
                          "stream-id layout must exactly tile the collective "
                          "tag window");
            const int base =
                ((parent_job->job_id % kJobIdWindow) * kMaxCGPerJob + cg_id) *
                kStreamsPerCG;
            cg->num_streams = base;
            cg->num_streams_base = base;
            comm_groups[cg_id] = cg;
        }
    }
    this->stats = new Statistics(this);
    this->is_finished = false;
}

Workload::~Workload() {
    for (auto comm_group : comm_groups) {
        delete comm_group.second;
    }
    comm_groups.clear();

    if (this->et_feeder != nullptr) {
        delete this->et_feeder;
    }
    if (this->hw_resource != nullptr) {
        delete this->hw_resource;
    }
    if (this->stats != nullptr) {
        delete this->stats;
    }
}

void Workload::initialize_comm_groups(string comm_group_filename) {
    // communicator group input file is not given
    if (comm_group_filename.find("empty") != std::string::npos) {
        comm_groups.clear();
        return;
    }

    ifstream inFile;
    json j;
    inFile.open(comm_group_filename);
    if (!inFile.is_open()) {
        LoggerFactory::get_logger("workload")
            ->critical("cannot open comm group file {} (errno={})",
                       comm_group_filename, errno);
        exit(EXIT_FAILURE);
    }
    try {
        inFile >> j;
    } catch (const std::exception& e) {
        LoggerFactory::get_logger("workload")
            ->critical("cannot parse comm group file {}: {}",
                       comm_group_filename, e.what());
        exit(EXIT_FAILURE);
    }

    for (json::iterator it = j.begin(); it != j.end(); ++it) {
        std::string comm_group_name = it.key();
        int comm_group_id = 0;
        try {
            comm_group_id = std::stoi(comm_group_name);
        } catch (const std::exception&) {
            LoggerFactory::get_logger("workload")
                ->critical("non-numeric comm group key '{}' in {}",
                           comm_group_name, comm_group_filename);
            exit(EXIT_FAILURE);
        }

        std::vector<int> involved_NPUs;
        for (auto id : it.value()) {
            involved_NPUs.push_back(id);
        }

        // ordered_rings (D4) is a scheduled-job concept; this path is also
        // reachable from the legacy constructor where parent_job is null, so
        // fall back to false (sorted ring = pre-change behavior).
        comm_groups[comm_group_id] = new CommunicatorGroup(
            comm_group_id, involved_NPUs, sys,
            (parent_job != nullptr ? parent_job->ordered_rings : false));
    }
}

void Workload::issue_pytorch_pg_metadata(
    std::shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    // For read comm groups from torch, might overwrite previous.
    std::string pg_info = node->get_inputs_values();
    if (pg_info.empty()) {
        return;
    }
    if (pg_info.size() < 4) {
        LoggerFactory::get_logger("workload")
            ->critical("rank {}: malformed pg_info '{}' in METADATA node",
                       sys->id, pg_info);
        std::exit(EXIT_FAILURE);
    }
    pg_info = pg_info.substr(2, pg_info.size() - 4);

    try {
        json valuesRoot = json::parse(pg_info);

        for (const auto& item : valuesRoot) {
            std::string pgName = item.at("pg_name").get<std::string>();
            std::vector<int> involved_NPUs =
                item.at("ranks").get<std::vector<int>>();

            if (involved_NPUs.empty()) {
                for (int i = 0; i < sys->total_nodes; i++) {
                    involved_NPUs.push_back(i);
                }
            }

            int32_t pgNameInt = std::stoi(pgName);
            // To ensure pgName > 0
            // parent_job may be null on the legacy (non-scheduled) path; fall
            // back to false so the ring sorts as before (D4 opt-in).
            // Multi-iteration jobs re-issue METADATA nodes each iteration;
            // free the previous group (and its lazily-built CollectivePlans)
            // instead of leaking it under the overwrite below. Safe at the
            // iteration boundary: a drained iteration has no live streams
            // referencing the old group.
            auto existing = this->comm_groups.find(pgNameInt);
            if (existing != this->comm_groups.end()) {
                delete existing->second;
            }
            CommunicatorGroup* cg = new CommunicatorGroup(
                pgNameInt + 1, involved_NPUs, sys,
                (parent_job != nullptr ? parent_job->ordered_rings : false));
            // Scheduled jobs: apply the same windowed (job, cg)-unique
            // stream-id base as the comm_group.json path above. The
            // constructor default (id * 1000000) both collides across jobs
            // and leaves the collective tag window for ids > 499.
            if (parent_job != nullptr) {
                if (pgNameInt < 0 || pgNameInt >= kMaxCGPerJob) {
                    LoggerFactory::get_logger("workload")
                        ->critical("job {}: pg metadata group id {} outside "
                                   "[0, {}) -- unsupported by the stream-id "
                                   "layout",
                                   parent_job->job_id, pgNameInt, kMaxCGPerJob);
                    std::exit(1);
                }
                const int base =
                    ((parent_job->job_id % kJobIdWindow) * kMaxCGPerJob +
                     pgNameInt) *
                    kStreamsPerCG;
                cg->num_streams = base;
                cg->num_streams_base = base;
            }
            this->comm_groups[pgNameInt] = cg;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing or processing JSON: " << e.what()
                  << std::endl;
    }
}

void Workload::issue_dep_free_nodes() {
    auto& dependancy_resolver = this->et_feeder->getDependancyResolver();

    // Some nodes complete synchronously inside issue() (skip_invalid:
    // METADATA and INVALID nodes, zero-tensor-size COMP): finish_node frees
    // their children mid-pass, but the children are not in this pass's
    // snapshot and a synchronous completion registers no event that would
    // re-enter this function. Without a re-scan, a rank whose only
    // remaining work was freed that way goes silent until the
    // global-quiescence re-issue -- effectively end-of-run under load.
    // Loop to a fix-point: re-snapshot while new dependency-free nodes
    // appeared during the pass. Terminates because a new appearance
    // requires finish_node, which permanently consumes a DAG node.
    bool rescan = true;
    while (rescan) {
        rescan = false;

        std::set<uint64_t> dependancy_free_nodes_set;
        for (const auto node_id :
             dependancy_resolver.get_dependancy_free_nodes()) {
            dependancy_free_nodes_set.insert(node_id);
        }

        bool success = true;
        for (const auto node_id : dependancy_free_nodes_set) {
            std::shared_ptr<ETFeederNode> node = et_feeder->lookupNode(node_id);
            // Grouped collectives must additionally be admitted in the
            // rank-invariant per-group order; see comm_admission_in_order. A
            // collective skipped here because a lower ordinal is still
            // pending is simply retried on a later call (this runs on every
            // event), so the gate delays admission but never strands a node.
            if (hw_resource->is_available(node) &&
                comm_admission_in_order(node)) {
                success = issue(node);
                if (success) {
                    mark_comm_admitted(node);
                } else {
                    auto logger = LoggerFactory::get_logger("workload");
                    logger->warn(
                        "Workload::issue failed, sys->id={}, node->id={}, "
                        "node->name={}, node->type={}",
                        sys->id, node->id(), node->name(),
                        static_cast<uint64_t>(node->type()));
                }
            }
        }

        for (const auto node_id :
             dependancy_resolver.get_dependancy_free_nodes()) {
            if (dependancy_free_nodes_set.count(node_id) == 0U) {
                rescan = true;
                break;
            }
        }
    }
}

bool Workload::comm_admission_in_order(
    shared_ptr<Chakra::FeederV3::ETFeederNode> node) const {
    if (node->type() != ChakraNodeType::COMM_COLL_NODE ||
        hw_resource->comm_cap_bypass) {
        return true;
    }
    const int cg_id = hw_resource->comm_group_of(node);
    if (cg_id < 0) {
        return true;
    }
    const auto cg_it = cg_node_to_ordinal_.find(cg_id);
    if (cg_it == cg_node_to_ordinal_.end()) {
        // Legacy path without a precomputed ordinal map: no ordering.
        return true;
    }
    const auto ord_it = cg_it->second.find(node->id());
    if (ord_it == cg_it->second.end()) {
        return true;
    }
    const auto unadmitted_it = cg_unadmitted_ordinals_.find(cg_id);
    if (unadmitted_it == cg_unadmitted_ordinals_.end() ||
        unadmitted_it->second.empty()) {
        return true;
    }
    return ord_it->second == *unadmitted_it->second.begin();
}

void Workload::mark_comm_admitted(
    shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    const int cg_id = hw_resource->comm_group_of(node);
    if (cg_id < 0) {
        return;
    }
    const auto cg_it = cg_node_to_ordinal_.find(cg_id);
    if (cg_it == cg_node_to_ordinal_.end()) {
        return;
    }
    const auto ord_it = cg_it->second.find(node->id());
    if (ord_it == cg_it->second.end()) {
        return;
    }
    cg_unadmitted_ordinals_[cg_id].erase(ord_it->second);
}

void Workload::reset_comm_admission_order() {
    cg_unadmitted_ordinals_.clear();
    for (const auto& cg_entry : cg_node_to_ordinal_) {
        auto& unadmitted = cg_unadmitted_ordinals_[cg_entry.first];
        for (const auto& node_ordinal : cg_entry.second) {
            unadmitted.insert(node_ordinal.second);
        }
    }
}

bool Workload::issue(shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    bool success = true;
    auto logger = LoggerFactory::get_logger("workload");
    // std::cout << "Workload::issue, sys->id=" << sys->id
    //           << ", tick=" << Sys::boostedTick()
    //           << ", node->id=" << node->id()
    //           << ", node->name=" << node->name()
    //           << ", node->type=" << static_cast<uint64_t>(node->type())
    //           << std::endl;

    if (sys->trace_enabled) {
        logger->debug("issue,sys->id={}, tick={}, node->id={}, "
                      "node->name={}, node->type={}",
                      sys->id, Sys::boostedTick(), node->id(), node->name(),
                      static_cast<uint64_t>(node->type()));
    }

    this->et_feeder->getDependancyResolver().take_node(node->id());
    this->hw_resource->occupy(node);
    // stats->record_end will be called in Workload::call
    stats->record_start(node, Sys::boostedTick());
    if (this->sys->track_local_mem) {
        this->local_mem_usage_tracker->recordStart(node, Sys::boostedTick());
    }
    if (sys->replay_only) {
        issue_replay(node);
    } else {
        if ((node->type() == ChakraNodeType::MEM_LOAD_NODE) ||
            (node->type() == ChakraNodeType::MEM_STORE_NODE)) {
            issue_remote_mem(node);
        } else if (node->type() == ChakraNodeType::COMP_NODE) {
            if (!this->sys->roofline_enabled) {
                issue_replay(node);

            } else {
                if (node->is_cpu_op<bool>(false)) {
                    // comp node on cpu
                    // should only appears in real system trace and should run
                    // with replay.
                    issue_replay(node);
                } else {
                    // comp node on gpu
                    issue_comp(node);
                }
            }
        } else if (node->type() == ChakraNodeType::COMM_COLL_NODE ||
                   node->type() == ChakraNodeType::COMM_SEND_NODE ||
                   node->type() == ChakraNodeType::COMM_RECV_NODE) {
            success = issue_comm(node);
            if (!success) {
                hw_resource->release(node);
                this->et_feeder->getDependancyResolver().push_back_node(
                    node->id());
                stats->record_end(node, Sys::boostedTick());
                if (this->sys->track_local_mem) {
                    this->local_mem_usage_tracker->recordEnd(
                        node, Sys::boostedTick());
                }
                return success;
            }
        } else if (node->type() == ChakraNodeType::INVALID_NODE) {
            skip_invalid(node);
        } else if (node->type() == ChakraNodeType::METADATA_NODE) {
            issue_metadata(node);
        } else {
            logger->critical("Unknown node type");
            exit(EXIT_FAILURE);
        }
    }
    return success;
}

void Workload::issue_metadata(shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    // TODO: someway to identify this metadata node is a pytorch pg node
    if (true) {
        issue_pytorch_pg_metadata(node);
    } else {
        throw std::runtime_error("Unknown metadata node type");
    }
    this->skip_invalid(node);  // for proper dependancy resolving
}

void Workload::issue_replay(shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
    wlhd->node_id = node->id();
    uint64_t runtime = 1ul;
    if (node->runtime() != 0ul) {
        // chakra runtimes are in microseconds and we should convert it into
        // nanoseconds
        runtime = node->runtime() * 1000;
    }
    if (node->is_cpu_op()) {
        hw_resource->tics_cpu_ops += runtime;
    } else {
        hw_resource->tics_gpu_ops += runtime;
    }
    // printf("Workload::issue_replay, sys->id=%d, node->id=%lu, runtime=%lu
    // ns\n",
    //        sys->id, node->id(), runtime);
    sys->register_event(this, EventType::General, wlhd, runtime);
}

void Workload::issue_remote_mem(
    shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
    wlhd->sys_id = sys->id;
    wlhd->workload = this;
    wlhd->node_id = node->id();
    sys->remote_mem->issue(node->tensor_size(), wlhd);
}

void Workload::issue_comp(shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    if (!this->sys->roofline_enabled) {
        throw std::runtime_error(
            "Roofline model is not enabled for non-replay comp");
    }

    if (node->is_cpu_op()) {
        throw std::runtime_error("Roofline is only available for GPU nodes");
        return;
    }

    WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
    wlhd->node_id = node->id();

    double num_ops = static_cast<double>(node->num_ops<uint64_t>());
    double tensor_size = static_cast<double>(node->tensor_size<uint64_t>());

    // if tensor_size is 0 during roofline mode, this is an invalid node
    if (tensor_size == 0) {
        skip_invalid(node);
        return;
    }

    double operational_intensity = num_ops / tensor_size;
    double perf = sys->roofline->get_perf(operational_intensity);
    double elapsed_time = static_cast<double>(node->num_ops()) / perf;  // sec
    uint64_t runtime = static_cast<uint64_t>(elapsed_time * 1e9);  // sec -> ns
    // if dummy node, use runtime from chakra directly
    if (node->name().find("DummyNode") != std::string::npos) {
        runtime = node->runtime() * 1e3;  // µs -> ns
    }

    if (node->is_cpu_op()) {
        hw_resource->tics_cpu_ops += runtime;
    } else {
        hw_resource->tics_gpu_ops += runtime;
    }
    sys->register_event(this, EventType::General, wlhd, runtime);

    auto& op_stat = this->stats->get_operator_statistics(node->id());
    op_stat.operation_intensity = operational_intensity;
    op_stat.compute_utilization = perf / sys->peak_perf;
    op_stat.memory_utilization =
        (perf / operational_intensity) / sys->local_mem_bw;
    op_stat.is_memory_bound = perf < sys->peak_perf;
    LoggerFactory::get_logger("workload")
        ->debug("operation_intensity={}, perf={}, elapsed_time={} "
                "compute_utilization={} memory_utilization={} tensor_size={} "
                "num_ops={}",
                operational_intensity, perf, elapsed_time,
                op_stat.compute_utilization.value(),
                op_stat.memory_utilization.value(), tensor_size, num_ops);
}

bool Workload::issue_comm(shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    if (node->is_cpu_op<bool>(false)) {
        throw std::runtime_error("Comm node should not be on CPU");
    }
    bool success = true;
    const auto node_type = node->type();
    if (node_type == ChakraNodeType::COMM_COLL_NODE) {
        success = this->issue_coll_comm(node);
    } else if (node_type == ChakraNodeType::COMM_SEND_NODE) {
        success = this->issue_send_comm(node);
    } else if (node_type == ChakraNodeType::COMM_RECV_NODE) {
        success = this->issue_recv_comm(node);
    } else {
        throw std::runtime_error("Unknown comm node type");
    }
    return success;
}

bool Workload::issue_coll_comm(
    shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    const bool has_involve_dims = node->has_attr("involve_dims");
    std::vector<bool> involved_dims;
    if (node->has_attr("involved_dim")) {
        const ChakraProtoMsg::AttributeProto& attr =
            node->get_attr_msg("involved_dim");

        // Ensure the attribute is of type bool_list before accessing
        if (attr.has_bool_list()) {
            const ChakraProtoMsg::BoolList& bool_list = attr.bool_list();

            // Traverse bool_list and add values to involved_dim
            for (int i = 0; i < bool_list.values_size(); ++i) {
                involved_dims.push_back(bool_list.values(i));
            }
        } else {
            cerr << "Expected bool_list in involved_dim but found another type."
                 << endl;
            exit(EXIT_FAILURE);
        }
    } else {
        // involved_dims does not exist in ETFeeder.
        // Assume involved_dims = [1,1,1,1,1] which we could simulate
        // 5-Dimension. Could use Process Group to build involved_dims later.
        // Once process group is implemented, you should get
        // that with node->pg_name()

        for (int i = 0; i < 4; i++) {
            involved_dims.push_back(true);
        }
    }

    CommunicatorGroup* comm_group = extract_comm_group(node);

    // Deterministic stream IDs: under non-contiguous placement, ranks
    // progress at different speeds and may have multiple collectives
    // in-flight on the same CG simultaneously.  The stream_id must be
    // invariant across ranks for the SAME logical collective so that
    // the network frontend's tag
    // matching can pair sends and receives.  We use the pre-computed
    // (cg_id, node_id) -> ordinal map (built from the sorted set of
    // node_ids per CG at construction time) — this is invariant across
    // ranks regardless of issue order.
    if (comm_group != nullptr) {
        int cg_id = comm_group->get_id();
        uint64_t nid = node->id();
        int ordinal = 0;
        auto cg_it = cg_node_to_ordinal_.find(cg_id);
        if (cg_it != cg_node_to_ordinal_.end()) {
            auto n_it = cg_it->second.find(nid);
            if (n_it != cg_it->second.end()) {
                ordinal = n_it->second;
            }
        }
        if (ordinal >= kStreamsPerCG / kMaxStreamsPerCollective) {
            LoggerFactory::get_logger("workload")
                ->critical("job {}: comm group {} has more than {} "
                           "collectives per iteration -- stream-id range "
                           "would bleed into the next comm group",
                           (parent_job != nullptr ? parent_job->job_id : -1),
                           cg_id, kStreamsPerCG / kMaxStreamsPerCollective);
            std::exit(1);
        }
        comm_group->num_streams =
            comm_group->num_streams_base + ordinal * kMaxStreamsPerCollective;
    }

    auto logger = LoggerFactory::get_logger("workload");
    // A pg-less collective is only meaningful in the legacy one-shot flow,
    // where every NPU in the system participates. In the scheduled flow the
    // dims-based fallback would enroll NPUs outside this job, so the
    // collective can never complete and the whole job wedges until global
    // quiescence. Fail fast with a diagnostic instead. (Mapping pg-less to
    // an implicit all-ranks group -- PyTorch default-pg semantics -- would
    // need the ordinal scan and admission gate to know about it; revisit if
    // real PyTorch traces become an input.)
    if (comm_group == nullptr && parent_job != nullptr) {
        logger->critical("job {} rank {}: COMM_COLL node {} has no pg_name; "
                         "pg-less collectives are unsupported in scheduled "
                         "mode",
                         parent_job->job_id, job_local_rank, node->id());
        std::exit(1);
    }
    // comm_group stays legitimately null on the legacy one-shot path (runs
    // under the ungrouped cap; generate_* handle a null group). spdlog
    // evaluates its arguments eagerly, so both the dereference and the
    // O(group-size) to_string() below must sit behind the null check and a
    // level check.
    if (comm_group != nullptr) {
        if (logger->should_log(spdlog::level::debug)) {
            logger->debug("RANK: {} Issuing collective {}", this->sys->id,
                          comm_group->to_string());
        }
        previous_group_id = comm_group->get_id();
    } else {
        logger->debug("RANK: {} Issuing ungrouped collective", this->sys->id);
    }

    sys->increment_inflight_coll();
    logger->debug("RANK: {} inflight collective count: {}", this->sys->id,
                  sys->get_inflight_coll());

    const auto comm_type =
        static_cast<ChakraCollectiveCommType>(node->comm_type<uint64_t>());
    const auto comm_size = node->comm_size<uint64_t>();
    stats->get_operator_statistics(node->id()).comm_size = comm_size;
    const auto comm_priority = node->comm_priority<uint32_t>();  // default 0u

    DataSet* fp = nullptr;
    if (comm_type == ChakraCollectiveCommType::ALL_REDUCE) {
        fp = sys->generate_all_reduce(comm_size, involved_dims, comm_group,
                                      comm_priority);
    } else if (comm_type == ChakraCollectiveCommType::ALL_TO_ALL) {
        fp = sys->generate_all_to_all(comm_size, involved_dims, comm_group,
                                      comm_priority);
    } else if (comm_type == ChakraCollectiveCommType::ALL_GATHER) {
        fp = sys->generate_all_gather(comm_size, involved_dims, comm_group,
                                      comm_priority);
    } else if (comm_type == ChakraCollectiveCommType::REDUCE_SCATTER) {
        fp = sys->generate_reduce_scatter(comm_size, involved_dims, comm_group,
                                          comm_priority);
    }
    if (fp != nullptr) {
        // A DataSet that produced no streams (all dims size 1 -- e.g. a
        // collective over a single-member comm group) is created inactive
        // and never notifies: the node would wait forever and the job would
        // wedge until global quiescence. No current trace generator emits
        // these; fail fast if one appears.
        if (!fp->active) {
            logger->critical("job {} rank {}: COMM_COLL node {} produced no "
                             "streams (single-member comm group?); "
                             "unsupported",
                             (parent_job != nullptr ? parent_job->job_id : -1),
                             job_local_rank, node->id());
            std::exit(1);
        }
        collective_comm_node_id_map[fp->my_id] = node->id();
        collective_comm_wrapper_map[fp->my_id] = fp;
        fp->set_notifier(this, EventType::CollectiveCommunicationFinished);
    } else if (comm_type == ChakraCollectiveCommType::BROADCAST) {
        uint64_t runtime = 1ul;
        if (node->runtime() != 0ul) {
            runtime = node->runtime() * 1000;
        }
        fp = new DataSet(1);
        fp->set_notifier(this, EventType::CollectiveCommunicationFinished);
        collective_comm_node_id_map[fp->my_id] = node->id();
        collective_comm_wrapper_map[fp->my_id] = fp;
        sys->register_event(fp, EventType::General, nullptr, runtime);
    } else {
        throw std::runtime_error("Unsupported collective comm type");
    }
    return true;
}

bool Workload::issue_send_comm(
    shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    const auto src = node->comm_src<uint32_t>(this->sys->id);
    if (src != this->sys->id) {
        throw std::runtime_error("Send node should be issued by the sender");
    }
    const auto dst = node->comm_dst<uint32_t>();
    int dst_npu = static_cast<int>(dst);
    if (parent_job != nullptr) {
        if (dst_npu >= static_cast<uint32_t>(parent_job->num_ranks)) {
            LoggerFactory::get_logger("workload")
                ->critical("job {} rank {}: COMM_SEND node {} names peer rank "
                           "{} outside [0, {})",
                           parent_job->job_id, job_local_rank, node->id(),
                           dst_npu, parent_job->num_ranks);
            std::exit(EXIT_FAILURE);
        }
        dst_npu = parent_job->rank_map[dst_npu];
    }
    const auto size = node->comm_size<uint64_t>();
    // Record communication size for bandwidth calculation
    stats->get_operator_statistics(node->id()).comm_size = size;
    const auto tag = node->comm_tag<uint32_t>();

    // stg_tp2_pp2
    int cur_comm_group_id = -1;
    auto logger = LoggerFactory::get_logger("workload");
    logger->debug("RANK: {} Issuing SEND {}-{}", this->sys->id, src, dst);
    previous_group_id = cur_comm_group_id;

    // sys->increment_inflight_coll();
    // std::cout << "RANK: " << this->sys->id << " inflight collective count: "
    // << sys->get_inflight_coll() << std::endl;

    sim_request snd_req;
    snd_req.srcRank = src;
    snd_req.dstRank = dst;
    snd_req.reqType = UINT8;
    SendPacketEventHandlerData* sehd = new SendPacketEventHandlerData;
    sehd->callable = this;
    sehd->wlhd = new WorkloadLayerHandlerData;
    sehd->wlhd->node_id = node->id();
    sehd->event = EventType::PacketSent;
    sys->front_end_sim_send(0, Sys::dummy_data, size, UINT8, dst_npu, tag,
                            &snd_req, Sys::FrontEndSendRecvType::NATIVE,
                            &Sys::handleEvent, sehd);
    return true;
}

bool Workload::issue_recv_comm(
    shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    const auto src = node->comm_src<uint32_t>();
    int src_npu = static_cast<int>(src);
    if (parent_job != nullptr) {
        if (src_npu >= static_cast<uint32_t>(parent_job->num_ranks)) {
            LoggerFactory::get_logger("workload")
                ->critical("job {} rank {}: COMM_RECV node {} names peer rank "
                           "{} outside [0, {})",
                           parent_job->job_id, job_local_rank, node->id(),
                           src_npu, parent_job->num_ranks);
            std::exit(EXIT_FAILURE);
        }
        src_npu = parent_job->rank_map[src_npu];
    }
    const auto dst = node->comm_dst<uint32_t>(this->sys->id);
    if (dst != this->sys->id) {
        throw std::runtime_error("Recv node should be issued by the receiver");
    }
    const auto size = node->comm_size<uint64_t>();
    // Record communication size for bandwidth calculation
    stats->get_operator_statistics(node->id()).comm_size = size;
    const auto tag = node->comm_tag<uint32_t>();

    // stg_tp2_pp2 no need to reconfigure because recv is paired with send
    // if(previous_group_id >= 0) {
    //     // TODO use suitable topo_id
    //     int topo_id = 1;
    //     bool can_config = sys->comm_NI->sim_reconfig(topo_id);
    //     if (!can_config) return false;
    //     std::cout << "RANK: " << this->sys->id << " Switching to SEND/RECV
    //     group " << std::endl;
    // }

    // previous_group_id = -1;

    auto logger = LoggerFactory::get_logger("workload");
    logger->debug("RANK: {} Issuing RECV {}-{}", this->sys->id, src, dst);

    sim_request rcv_req;
    RecvPacketEventHandlerData* rcehd = new RecvPacketEventHandlerData;
    rcehd->wlhd = new WorkloadLayerHandlerData;
    rcehd->wlhd->node_id = node->id();
    rcehd->workload = this;
    rcehd->event = EventType::PacketReceived;
    sys->front_end_sim_recv(0, Sys::dummy_data, size, UINT8, src_npu, tag,
                            &rcv_req, Sys::FrontEndSendRecvType::NATIVE,
                            &Sys::handleEvent, rcehd);
    return true;
}

void Workload::skip_invalid(shared_ptr<Chakra::FeederV3::ETFeederNode> node) {
    const auto node_id = node->id();
    auto& dependancy_resolver = this->et_feeder->getDependancyResolver();
    dependancy_resolver.finish_node(node_id);
    auto logger = LoggerFactory::get_logger("workload");
    logger->debug("callback,sys->id={}, tick={}, node->id={}, "
                  "node->name={}, node->type={}",
                  sys->id, Sys::boostedTick(), node->id(), node->name(),
                  static_cast<uint64_t>(node->type()));
    hw_resource->release(node);
    stats->record_end(node, Sys::boostedTick());
    if (this->sys->track_local_mem) {
        this->local_mem_usage_tracker->recordEnd(node, Sys::boostedTick());
    }
}

void Workload::call(EventType event, CallData* data) {
    auto logger = LoggerFactory::get_logger("workload");
    if (is_finished) {
        logger->debug("Rank {}: workload already finished, ignore event {}",
                      this->sys->id, static_cast<int>(event));
        // Deliberately does NOT delete `data`: payload ownership is mixed
        // (DataSet deletes the IntData it passes right after call() returns;
        // other paths expect the callee to free), so a blanket delete here
        // would double-free collective completions. The path is defensive
        // and unreachable under the drain invariant; the potential leak is
        // accepted.
        return;
    }

    if (event == EventType::CollectiveCommunicationFinished) {
        IntData* int_data = (IntData*)data;
        uint64_t coll_comm_id = int_data->data;
        sys->decrement_inflight_coll();
        logger->debug("RANK: {} finish collective: {}, inflight collective {}",
                      this->sys->id, coll_comm_id, sys->get_inflight_coll());

        // if (current_comm_group_idx < comm_group_list.size()) {
        //     int next_comm_group_id = comm_group_list[current_comm_group_idx];
        //     int topo_id = 0;
        //     if (next_comm_group_id == 3 || next_comm_group_id == 4) {
        //         topo_id = 1;
        //     }
        //     bool can_config = sys->comm_NI->sim_reconfig(topo_id);
        //     printf("RANK: %d attempted to provision to topo_id: %d, result:
        //     %d\n", this->sys->id, topo_id, can_config);
        // }

        // if (coll_comm_id == 1921) {
        //     // TODO change coll_comm_id
        //     int topo_id = 0;
        //     bool can_config = sys->comm_NI->sim_reconfig(topo_id);
        //     printf("RANK: %d hard-code attempted to provision to topo_id: %d,
        //     result: %d\n", this->sys->id, topo_id, can_config);
        // }

        hw_resource->tics_gpu_comms += int_data->execution_time;
        // .at(): a miss here means the completion event has no issued
        // collective behind it; fail loudly instead of operator[]'s silent
        // default-insert of node id 0.
        uint64_t node_id = collective_comm_node_id_map.at(coll_comm_id);
        shared_ptr<Chakra::FeederV3::ETFeederNode> node =
            et_feeder->lookupNode(node_id);

        if (sys->trace_enabled) {
            LoggerFactory::get_logger("workload")
                ->debug("callback,sys->id={}, tick={}, node->id={}, "
                        "node->name={}, node->type={}",
                        sys->id, Sys::boostedTick(), node->id(), node->name(),
                        static_cast<uint64_t>(node->type()));
        }

        hw_resource->release(node);
        stats->record_end(node, Sys::boostedTick());

        // Calculate network bandwidth
        auto& op_stat = stats->get_operator_statistics(node_id);
        Tick execution_time = int_data->execution_time;
        if (execution_time > 0 && op_stat.comm_size.has_value()) {
            double bandwidth =
                static_cast<double>(op_stat.comm_size.value()) / execution_time;
            op_stat.network_bandwidth = bandwidth;
        }

        if (this->sys->track_local_mem) {
            this->local_mem_usage_tracker->recordEnd(node, Sys::boostedTick());
        }

        this->et_feeder->getDependancyResolver().finish_node(node_id);

        issue_dep_free_nodes();

        // The Dataset class provides statistics that should be used later to
        // dump more statistics in the workload layer
        delete collective_comm_wrapper_map[coll_comm_id];
        collective_comm_wrapper_map.erase(coll_comm_id);
        // Erase the node-id mapping with its wrapper; it previously grew by
        // one entry per collective per iteration for the job's lifetime.
        collective_comm_node_id_map.erase(coll_comm_id);

    } else {
        if (data == nullptr) {
            issue_dep_free_nodes();
        } else {
            WorkloadLayerHandlerData* wlhd = (WorkloadLayerHandlerData*)data;
            shared_ptr<Chakra::FeederV3::ETFeederNode> node =
                et_feeder->lookupNode(wlhd->node_id);

            if (sys->trace_enabled) {
                LoggerFactory::get_logger("workload")
                    ->debug("callback,sys->id={}, tick={}, node->id={}, "
                            "node->name={}, node->type={}",
                            sys->id, Sys::boostedTick(), node->id(),
                            node->name(), static_cast<uint64_t>(node->type()));
            }

            hw_resource->release(node);
            stats->record_end(node, Sys::boostedTick());

            // Calculate network bandwidth for point-to-point communications
            if (event == EventType::PacketSent ||
                event == EventType::PacketReceived) {

                logger->debug("RANK: {} finish SEND/RECV", this->sys->id);

                auto& op_stat = stats->get_operator_statistics(wlhd->node_id);
                Tick execution_time =
                    stats->get_operator_statistics(wlhd->node_id).end_time -
                    stats->get_operator_statistics(wlhd->node_id).start_time;
                if (execution_time > 0 && op_stat.comm_size.has_value()) {
                    double bandwidth =
                        static_cast<double>(op_stat.comm_size.value()) /
                        execution_time;
                    op_stat.network_bandwidth = bandwidth;
                }
            }

            if (this->sys->track_local_mem) {
                this->local_mem_usage_tracker->recordEnd(node,
                                                         Sys::boostedTick());
            }

            this->et_feeder->getDependancyResolver().finish_node(wlhd->node_id);

            issue_dep_free_nodes();

            delete wlhd;
        }
    }

    // Iteration / job completion. The single-iteration trace is replayed
    // total_iterations_ times to model a multi-iteration training job. An
    // iteration is "done" once its DAG fully drains (current_iteration_drained
    // below). On each drain we either advance to the next iteration or, after
    // the last one, finish the job for good.
    //
    // The loop re-checks the predicate after an advance: a normal iteration
    // issues real work whose synchronous occupy() makes the predicate false,
    // so the loop runs once and exits, with future events driving the new
    // iteration. The loop only spins when an iteration issued no work at all
    // (degenerate/empty trace) -- without the re-check such an iteration would
    // wedge the rank with no pending event to wake it.
    while (current_iteration_drained()) {
        if (current_iteration_ + 1 < total_iterations_) {
            ++current_iteration_;
            advance_to_next_iteration();
            issue_dep_free_nodes();
        } else {
            finish();
            break;
        }
    }
}

bool Workload::current_iteration_drained() const {
    const auto& dep_resolver = this->et_feeder->getDependancyResolver();
    return dep_resolver.get_dependancy_free_nodes().empty() &&
           dep_resolver.get_ongoing_nodes().empty() &&
           hw_resource->num_in_flight_cpu_ops == 0 &&
           hw_resource->num_in_flight_gpu_comp_ops == 0 &&
           hw_resource->num_in_flight_gpu_comm_ops == 0;
}

void Workload::advance_to_next_iteration() {
    // Rewind the feeder's dependency resolver to the pristine snapshot taken
    // at construction: same effect as rebuilding the feeder from the trace
    // file (a fresh resolver re-seeded from the DAG roots) without the
    // per-iteration protobuf re-parse, and the feeder-id-keyed node cache
    // stays warm. cg_node_to_ordinal_ is invariant across iterations and is
    // NOT recomputed; hw_resource keeps its tics_* accumulators (whole-job
    // totals) while its in-flight counters are already 0 at a drain boundary.
    //
    // We deliberately REUSE every logical id rather than minting new ones,
    // which is safe precisely because the previous iteration is fully drained
    // before we get here:
    //   * comm-group ids are trace-defined; the CommunicatorGroup objects in
    //     comm_groups persist across iterations untouched.
    //   * collective stream-id bases are a deterministic function of a
    //     collective's node ordinal -- issue_coll_comm() re-derives
    //     num_streams = num_streams_base + ordinal * kMaxStreamsPerCollective
    //     on every issue -- so the next iteration reproduces identical stream
    //     ids. No stream with those ids is still live, and no global state
    //     is keyed by stream id (the once-write-only BaseStream::synchronizer
    //     bookkeeping was removed together with its dead reader).
    //   * send/recv tags are trace-defined and paired-and-cleared within an
    //     iteration before the next one begins.
    // The only monotonic ids in play are transient DataSet ids (the keys of
    // collective_comm_*_map), drawn from a global counter; letting them climb
    // across iterations is correct and collision-free, and the maps are empty
    // here because a drained iteration has no in-flight collectives.
    this->et_feeder->reset_dependancy();

    // Node ids repeat across iterations, so per-group admission order starts
    // over from the first ordinal.
    reset_comm_admission_order();

    // Should be unreachable (a drained iteration has 0 in-flight comm ops, so
    // every wrapper was erased on completion). Surface a straggler instead of
    // silently leaking it rather than asserting.
    if (!collective_comm_wrapper_map.empty()) {
        LoggerFactory::get_logger("workload")
            ->warn("rank {}: {} collective wrapper(s) still live at the "
                   "boundary into iteration {}",
                   sys->id, collective_comm_wrapper_map.size(),
                   current_iteration_);
    }
}

void Workload::finish() {
    report();
    sys->comm_NI->sim_notify_finished();
    is_finished = true;
    if (parent_job != nullptr) {
        parent_job->on_rank_finished(job_local_rank, Sys::boostedTick());
    }
}

void Workload::fire() {
    call(EventType::General, NULL);
}

void Workload::report() {
    Tick curr_tick = Sys::boostedTick();
    LoggerFactory::get_logger("workload")
        ->info("sys[{}] finished, {} cycles, exposed communication {} cycles.",
               sys->id, curr_tick, curr_tick - hw_resource->tics_gpu_ops);
    stats->post_processing();
    stats->report();
    if (this->sys->track_local_mem) {
        this->local_mem_usage_tracker->buildMemoryTrace();
        this->local_mem_usage_tracker->buildMemoryTimeline();
        this->local_mem_usage_tracker->dumpMemoryTrace(
            this->sys->local_mem_trace_filename);
        auto [peak_mem_usage, unit] =
            this->local_mem_usage_tracker->getPeakMemUsageFormatted();
        auto logger = LoggerFactory::get_logger("workload");
        logger->info("sys[{}] peak memory usage: {:.2f} {}", sys->id,
                     peak_mem_usage, unit);
        this->local_mem_usage_tracker.reset();
    }
}

CommunicatorGroup* Workload::extract_comm_group(
    std::shared_ptr<Chakra::ETFeederNode> node) {
    std::string comm_group_name = node->pg_name<std::string>("");
    if (comm_group_name == "") {
        // No communicator group is specified for this communication ET node.
        return nullptr;
    }
    int comm_group_id = 0;
    try {
        comm_group_id = std::stoi(comm_group_name);
    } catch (const std::exception&) {
        LoggerFactory::get_logger("workload")
            ->critical("rank {} ET node {}: non-numeric pg_name '{}'", sys->id,
                       node->id(), comm_group_name);
        exit(EXIT_FAILURE);
    }
    if (comm_groups.find(comm_group_id) == comm_groups.end()) {
        LoggerFactory::get_logger("workload")
            ->critical(
                "For rank {} ET node {}, communicator group {} not found",
                sys->id, node->id(), comm_group_id);
        exit(EXIT_FAILURE);
    }
    return comm_groups[comm_group_id];
}
