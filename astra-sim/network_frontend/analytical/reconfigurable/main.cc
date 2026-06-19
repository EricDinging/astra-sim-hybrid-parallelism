/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/common/Logging.hh"
#include "astra-sim/scheduling/AdmissionPolicy.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"
#include "astra-sim/scheduling/FailureSelection.hh"
#include "astra-sim/scheduling/FragmentationScorer.hh"
#include "astra-sim/scheduling/JobArrivalFile.hh"
#include "astra-sim/scheduling/JobStatsWriter.hh"
#include "astra-sim/scheduling/PlacementPolicy.hh"
#include "astra-sim/scheduling/SchedRuntime.hh"
#include "astra-sim/system/BaseStream.hh"  // DEBUG(deadlock-repro)
#include "common/CmdLineParser.hh"
#include "reconfigurable/ReconfigurableNetworkApi.hh"
#include <astra-network-analytical/common/EventQueue.h>
#include <astra-network-analytical/common/NetworkParser.h>
#include <astra-network-analytical/reconfigurable/Helper.h>
#include <json/json.hpp>
#include <remote_memory_backend/analytical/AnalyticalRemoteMemory.hh>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

using namespace AstraSim;
using namespace Analytical;
using namespace AstraSimAnalytical;
using namespace AstraSimAnalyticalReconfigurable;
using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

using bw_matrix_t = std::vector<std::vector<Bandwidth>>;
using lt_matrix_t = std::vector<std::vector<Latency>>;

namespace {
// Applies an rfold ReconfigPlan to the backend TopologyManager: wire the OCS
// edges at the per-link rate and install the 1-hop routes. Scoped, no drain.
class TmReconfigHook : public AstraSim::Scheduling::ReconfigHook {
  public:
    TmReconfigHook(std::shared_ptr<TopologyManager> tm,
                   Bandwidth bw,
                   Latency lt)
        : tm_(std::move(tm)),
          bw_(bw),
          lt_(lt) {}
    void apply(const AstraSim::Scheduling::ReconfigPlan& plan) override {
        std::vector<std::vector<int>> routes;
        routes.reserve(plan.routes.size());
        for (const auto& row : plan.routes) {
            routes.push_back(row.hops);
        }
        tm_->apply_job_wiring(plan.ocs_edges, routes, bw_, lt_);
    }

  private:
    std::shared_ptr<TopologyManager> tm_;
    Bandwidth bw_;
    Latency lt_;
};
}  // namespace

static inline std::string trim(const std::string& s) {
    auto ws = " \t\r\n";
    size_t start = s.find_first_not_of(ws);
    if (start == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

template <typename T>
std::map<int, std::vector<std::vector<T>>> parse_schedule_file(
    const std::string& filename, const std::string& tag) {
    std::map<int, std::vector<std::vector<T>>> schedules;
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[Error] (AstraSim/analytical/reconfigurable) "
                  << "Cannot open schedule file: " << filename << std::endl;
        std::exit(1);
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line.rfind("//", 0) == 0) {
            continue;
        }

        if (line.substr(0, 2) == tag) {
            int topo_id = std::stoi(line.substr(3));
            std::vector<std::vector<T>> matrix;

            while (std::getline(file, line)) {
                line = trim(line);
                if (line.empty() || line.rfind("//", 0) == 0) {
                    continue;
                }
                if (line == "END") {
                    break;
                }
                std::istringstream ss(line);
                T value;
                std::vector<T> row;
                while (ss >> value) {
                    row.push_back(value);
                }
                matrix.push_back(row);
            }
            schedules[topo_id] = matrix;

            auto logger = AstraSim::LoggerFactory::get_logger("default");
            logger->debug("Parsed {} matrix for topology: {}", tag, topo_id);
            for (const auto& row : matrix) {
                std::string msg;
                for (const auto& v : row) {
                    msg += std::to_string(v) + " ";
                }
                logger->debug("{}", msg);
            }
        }
    }
    return schedules;
}

static void validate_bw_schedules(
    const std::map<int, bw_matrix_t>& bw_schedules) {
    if (bw_schedules.empty()) {
        std::cerr << "[Error] (AstraSim/analytical/reconfigurable) "
                  << "BW schedule file contains no BW blocks" << std::endl;
        std::exit(1);
    }
    if (bw_schedules.find(0) == bw_schedules.end()) {
        std::cerr << "[Error] (AstraSim/analytical/reconfigurable) "
                  << "BW schedule must include topo_id 0 (initial topology)"
                  << std::endl;
        std::exit(1);
    }
    for (const auto& [topo_id, mat] : bw_schedules) {
        const size_t N = mat.size();
        for (size_t i = 0; i < mat.size(); ++i) {
            if (mat[i].size() != N) {
                std::cerr << "[Error] (AstraSim/analytical/reconfigurable) "
                          << "BW matrix not square for topo_id " << topo_id
                          << ": row " << i << " has " << mat[i].size()
                          << " cols, expected " << N << std::endl;
                std::exit(1);
            }
            for (size_t j = 0; j < mat[i].size(); ++j) {
                if (mat[i][j] < 0) {
                    std::cerr << "[Error] (AstraSim/analytical/reconfigurable) "
                              << "Negative bandwidth in topo_id " << topo_id
                              << " at (" << i << "," << j << "): " << mat[i][j]
                              << std::endl;
                    std::exit(1);
                }
            }
        }
    }
}

static void validate_latency_match(
    const std::map<int, bw_matrix_t>& bw_schedules,
    const std::map<int, lt_matrix_t>& latency_schedules) {
    if (bw_schedules.size() != latency_schedules.size()) {
        std::cerr << "[Error] (AstraSim/analytical/reconfigurable) "
                  << "BW/LT topo_id count mismatch: BW=" << bw_schedules.size()
                  << " LT=" << latency_schedules.size() << std::endl;
        std::exit(1);
    }
    for (const auto& [topo_id, bw_mat] : bw_schedules) {
        auto it = latency_schedules.find(topo_id);
        if (it == latency_schedules.end()) {
            std::cerr << "[Error] (AstraSim/analytical/reconfigurable) "
                      << "LT schedule missing topo_id " << topo_id << std::endl;
            std::exit(1);
        }
        const auto& lt_mat = it->second;
        if (lt_mat.size() != bw_mat.size()) {
            std::cerr << "[Error] (AstraSim/analytical/reconfigurable) "
                      << "LT matrix row count mismatch for topo_id " << topo_id
                      << ": LT=" << lt_mat.size() << " BW=" << bw_mat.size()
                      << std::endl;
            std::exit(1);
        }
        for (size_t i = 0; i < lt_mat.size(); ++i) {
            if (lt_mat[i].size() != bw_mat[i].size()) {
                std::cerr << "[Error] (AstraSim/analytical/reconfigurable) "
                          << "LT matrix col count mismatch for topo_id "
                          << topo_id << " row " << i
                          << ": LT=" << lt_mat[i].size()
                          << " BW=" << bw_mat[i].size() << std::endl;
                std::exit(1);
            }
            for (size_t j = 0; j < lt_mat[i].size(); ++j) {
                if (lt_mat[i][j] < 0) {
                    std::cerr << "[Error] (AstraSim/analytical/reconfigurable) "
                              << "Negative latency in topo_id " << topo_id
                              << " at (" << i << "," << j
                              << "): " << lt_mat[i][j] << std::endl;
                    std::exit(1);
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    auto cmd_line_parser = CmdLineParser(argv[0]);
    cmd_line_parser.parse(argc, argv);

    // Get command line arguments
    const auto system_configuration =
        cmd_line_parser.get<std::string>("system-configuration");
    const auto remote_memory_configuration =
        cmd_line_parser.get<std::string>("remote-memory-configuration");
    const auto network_configuration =
        cmd_line_parser.get<std::string>("network-configuration");

    const auto bw_schedule_path =
        cmd_line_parser.get<std::string>("bw-schedule");
    const auto latency_schedule_path =
        cmd_line_parser.get<std::string>("latency-schedule");

    const auto logging_configuration =
        cmd_line_parser.get<std::string>("logging-configuration");
    const auto logging_folder =
        cmd_line_parser.get<std::string>("logging-folder");
    const auto num_queues_per_dim =
        cmd_line_parser.get<int>("num-queues-per-dim");
    const auto comm_scale = cmd_line_parser.get<double>("comm-scale");
    const auto injection_scale = cmd_line_parser.get<double>("injection-scale");
    const auto rendezvous_protocol =
        cmd_line_parser.get<bool>("rendezvous-protocol");
    const auto npus_per_dim_str =
        cmd_line_parser.get<std::string>("npus-per-dim");

    const auto job_arrival_file =
        cmd_line_parser.get<std::string>("job-arrival-file");
    const auto jobs_dir = cmd_line_parser.get<std::string>("jobs-dir");
    const auto placement_policy =
        cmd_line_parser.get<std::string>("placement-policy");
    const auto admission_policy =
        cmd_line_parser.get<std::string>("admission-policy");
    const auto duration_estimator =
        cmd_line_parser.get<std::string>("duration-estimator");
    const auto failure_prob = cmd_line_parser.get<double>("failure-prob");

    AstraSim::LoggerFactory::init(logging_configuration, logging_folder);

    auto logger = AstraSim::LoggerFactory::get_logger("default");

    if (job_arrival_file.empty() || jobs_dir.empty()) {
        logger->critical("both --job-arrival-file and --jobs-dir are required");
        std::exit(1);
    }

    const auto event_queue = std::make_shared<EventQueue>();
    Topology::set_event_queue(event_queue);

    const auto network_parser = NetworkParser(network_configuration);
    const auto npus_count = network_parser.get_npus_counts_per_dim()[0];
    const Latency scalar_latency = network_parser.get_latencies_per_dim()[0];
    const Latency reconfig_latency = network_parser.get_reconfig_time();

    logger->debug("Parsing BW schedule from {}", bw_schedule_path);
    auto bw_schedules = parse_schedule_file<Bandwidth>(bw_schedule_path, "BW");
    validate_bw_schedules(bw_schedules);

    // Max link bandwidth across all bandwidth-schedule matrices (GB/s ->
    // bytes/s); the duration estimator's comm term divides by this. Computed
    // here because bw_schedules is moved into the TopologyManager below.
    double max_link_bw_bytes_per_s = 0.0;
    {
        double max_bw_gbps = 0.0;
        for (const auto& [topo_id, mat] : bw_schedules) {
            for (const auto& row : mat) {
                for (Bandwidth bw : row) {
                    if (bw > max_bw_gbps) {
                        max_bw_gbps = bw;
                    }
                }
            }
        }
        max_link_bw_bytes_per_s = max_bw_gbps * 1e9;
    }

    // Per-link rate for rfold's OCS links: the homogeneous per-link bandwidth /
    // latency declared in network.yml (link_bw_gbps + scalar_latency). Under
    // the homogeneous-cube model these equal the baseline torus ICI links, so
    // an OCS link behaves exactly like a hardwired link. Both come from the
    // same source (network.yml per-dim), so they stay consistent regardless of
    // the bw/latency-schedule files. A heterogeneous per-link schedule is not
    // modeled for OCS links.
    const double link_bw_gbps = network_parser.get_bandwidths_per_dim()[0];

    std::map<int, lt_matrix_t> latency_schedules;
    if (!latency_schedule_path.empty()) {
        logger->debug("Parsing LT schedule from {}", latency_schedule_path);
        latency_schedules =
            parse_schedule_file<Latency>(latency_schedule_path, "LT");
        validate_latency_match(bw_schedules, latency_schedules);
    } else {
        logger->debug(
            "No --latency-schedule provided; synthesizing uniform latency {} "
            "ns per topology from network.yml",
            scalar_latency);
        for (const auto& [topo_id, bw_mat] : bw_schedules) {
            const size_t N = bw_mat.size();
            latency_schedules[topo_id] =
                lt_matrix_t(N, std::vector<Latency>(N, scalar_latency));
        }
    }

    std::vector<int> npus_per_dim;
    if (!npus_per_dim_str.empty()) {
        std::istringstream ss(npus_per_dim_str);
        std::string token;
        while (std::getline(ss, token, ',')) {
            int dim = std::stoi(token);
            if (dim > 1) {
                npus_per_dim.push_back(dim);
            }
        }
    }

    if ((placement_policy == "firstfit" || placement_policy == "topomatch") &&
        npus_per_dim.size() != 3) {
        logger->critical("--placement-policy={} requires --npus-per-dim with "
                         "exactly 3 dims",
                         placement_policy);
        std::exit(1);
    }
    if (!npus_per_dim.empty()) {
        int prod = 1;
        for (int d : npus_per_dim) {
            prod *= d;
        }
        if (prod != npus_count) {
            logger->critical("--npus-per-dim product {} != npus_count {}", prod,
                             npus_count);
            std::exit(1);
        }
    }

    // Select failed NPUs once, before any topology/placement setup. Seed is
    // fixed at 42 for reproducibility (not a CLI knob). Empty when
    // --failure-prob is 0, in which case routing and placement are unaffected.
    const std::vector<int> failed_npus_vec =
        AstraSim::Scheduling::select_failed_npus(npus_count, failure_prob,
                                                 /*seed=*/42);
    // Consumed by TopologyManager (routing, below) and SchedRuntime
    // (placement).
    const std::unordered_set<int> failed_npus_set(failed_npus_vec.begin(),
                                                  failed_npus_vec.end());
    if (!failed_npus_vec.empty()) {
        logger->info("failure-prob {} -> {} of {} NPUs failed", failure_prob,
                     failed_npus_vec.size(), npus_count);
    }

    auto tm = std::make_shared<TopologyManager>(
        npus_count, npus_count, event_queue.get(), std::move(bw_schedules),
        std::move(latency_schedules), npus_per_dim, !npus_per_dim.empty());

    tm->set_failed_npus(failed_npus_set);
    tm->reconfigure(0);
    tm->set_reconfig_latency(reconfig_latency);

    // Set up Network API
    ReconfigurableNetworkApi::set_event_queue(event_queue);
    ReconfigurableNetworkApi::set_topology(tm);

    // Create ASTRA-sim related resources
    auto network_apis =
        std::vector<std::unique_ptr<ReconfigurableNetworkApi>>();

    const auto memory_api =
        std::make_unique<AnalyticalRemoteMemory>(remote_memory_configuration);

    auto queues_per_dim = std::vector<int>(1, num_queues_per_dim);
    std::vector<AstraSim::Sys*> systems;
    for (int i = 0; i < npus_count; ++i) {
        auto network_api = std::make_unique<ReconfigurableNetworkApi>(i);
        auto* sys =
            new AstraSim::Sys(i, system_configuration, memory_api.get(),
                              network_api.get(), {npus_count}, queues_per_dim,
                              injection_scale, comm_scale, rendezvous_protocol);
        network_apis.push_back(std::move(network_api));
        systems.push_back(sys);
    }

    const auto defrag_metric =
        cmd_line_parser.get<std::string>("defrag-metric");
    const auto block_size_str = cmd_line_parser.get<std::string>("block-size");
    AstraSim::Scheduling::PlacementConfig placement_cfg;
    placement_cfg.defrag_metric = defrag_metric;
    if (!AstraSim::Scheduling::parse_block_size(block_size_str,
                                                placement_cfg.block_size)) {
        logger->critical("invalid --block-size '{}' (want AxBxC)",
                         block_size_str);
        std::exit(1);
    }
    placement_cfg.rfold_selector =
        cmd_line_parser.get<std::string>("rfold-selector");
    placement_cfg.rfold_search_budget =
        cmd_line_parser.get<int>("rfold-search-budget");
    placement_cfg.rfold_ranking =
        cmd_line_parser.get<std::string>("rfold-ranking");
    placement_cfg.rfold_multifold =
        cmd_line_parser.get<bool>("rfold-multifold");
    placement_cfg.cm_kappa = cmd_line_parser.get<double>("cost-model-kappa");
    placement_cfg.cm_c_ext = cmd_line_parser.get<double>("cost-model-c-ext");
    placement_cfg.cm_t_reconfig_s =
        cmd_line_parser.get<double>("cost-model-t-reconfig");
    placement_cfg.cm_gamma = cmd_line_parser.get<double>("cost-model-gamma");
    placement_cfg.cm_lookahead_k =
        cmd_line_parser.get<int>("cost-model-lookahead-k");
    placement_cfg.cm_lookahead_q =
        cmd_line_parser.get<int>("cost-model-lookahead-q");
    placement_cfg.cm_beta_const_s =
        cmd_line_parser.get<double>("cost-model-beta-const");
    placement_cfg.cm_scatter_guard =
        cmd_line_parser.get<bool>("cost-model-scatter-guard");
    placement_cfg.switch_theta = cmd_line_parser.get<int>("switch-theta");
    auto policy = AstraSim::Scheduling::make_placement_policy(placement_policy,
                                                              placement_cfg);
    if (!policy) {
        logger->critical(
            "unknown --placement-policy '{}' (or, for rfold, an unknown "
            "--defrag-metric '{}', --rfold-selector '{}', or --rfold-ranking "
            "'{}')",
            placement_policy, defrag_metric, placement_cfg.rfold_selector,
            placement_cfg.rfold_ranking);
        std::exit(1);
    }
    auto arrivals =
        AstraSim::Scheduling::JobArrivalFile::parse(job_arrival_file);
    AstraSim::Scheduling::JobStatsWriter writer(logging_folder);
    writer.emit_failed_nodes(failed_npus_vec);

    // Build the admission config (used by sjdf/ljdf/swf/easy/lwf; fifo, sjsf,
    // and ljsf ignore it).
    AstraSim::Scheduling::AdmissionConfig admission_cfg;
    admission_cfg.estimator_name = duration_estimator;
    admission_cfg.max_link_bw = max_link_bw_bytes_per_s;
    // Peak compute + local memory bandwidth from the system config, matching
    // Sys.cc unit conventions (TFLOP/s -> FLOP/s, GB/s -> bytes/s).
    {
        std::ifstream sys_in(system_configuration);
        if (sys_in) {
            nlohmann::json sys_j;
            sys_in >> sys_j;
            if (sys_j.contains("peak-perf")) {
                admission_cfg.peak_perf =
                    static_cast<double>(sys_j["peak-perf"]) * 1e12;
            }
            if (sys_j.contains("local-mem-bw")) {
                admission_cfg.local_mem_bw =
                    static_cast<double>(sys_j["local-mem-bw"]) * 1e9;
            }
        }
    }
    // Duration-based admission AND the cost-model placement ranking both
    // build a roofline estimator; either path needs positive rates (a zero
    // peak-perf would make the estimator divide by zero). `switch` reads
    // only queue depth, so it needs no estimator.
    const bool ranking_needs_estimator =
        placement_policy == "rfold" &&
        placement_cfg.rfold_ranking == "cost-model";
    if ((admission_policy == "sjdf" || admission_policy == "ljdf" ||
         admission_policy == "swf" || admission_policy == "easy" ||
         admission_policy == "lwf" || ranking_needs_estimator) &&
        (admission_cfg.peak_perf <= 0.0 || admission_cfg.local_mem_bw <= 0.0 ||
         admission_cfg.max_link_bw <= 0.0)) {
        logger->critical(
            "--admission-policy={} / --rfold-ranking={} requires peak-perf "
            "and local-mem-bw in --system-configuration and a positive "
            "bandwidth schedule",
            admission_policy, placement_cfg.rfold_ranking);
        std::exit(1);
    }
    auto admission = AstraSim::Scheduling::make_admission_policy(
        admission_policy, admission_cfg);
    if (!admission) {
        logger->critical("unknown --admission-policy '{}' (or unsupported "
                         "--duration-estimator '{}')",
                         admission_policy, duration_estimator);
        std::exit(1);
    }

    AstraSim::Scheduling::SchedRuntime runtime(
        event_queue.get(), systems, npus_per_dim, std::move(policy),
        std::move(admission), std::move(arrivals), jobs_dir, &writer);
    TmReconfigHook reconfig_hook(tm, Bandwidth(link_bw_gbps),
                                 Latency(scalar_latency));
    runtime.set_reconfig_hook(&reconfig_hook);
    runtime.set_failed_npus(failed_npus_set);
    // Context-aware rankings need per-job duration estimates regardless of
    // the admission policy; reuse the same roofline estimator parameters
    // (validated above when ranking_needs_estimator).
    if (ranking_needs_estimator) {
        runtime.set_ctx_estimator(AstraSim::Scheduling::make_duration_estimator(
            duration_estimator, admission_cfg.peak_perf,
            admission_cfg.local_mem_bw, admission_cfg.max_link_bw));
    }
    // Deadlock post-mortem (fires only if the liveness valve in
    // SchedRuntime::run could not make progress): dump every non-empty
    // pending-chunk queue, unresolved send/recv matcher entries, and each
    // busy Sys's stream state, so the wait-for structure is visible.
    runtime.set_drain_diagnostic([&tm, &systems, npus_count]() {
        const int n = npus_count;
        std::cerr << "=== DRAIN DIAGNOSTIC: pending chunk queues ===\n";
        for (int i = 0; i < n; ++i) {
            auto dev = tm->get_device(i);
            for (int j = 0; j < n; ++j) {
                if (i == j) {
                    continue;
                }
                const int cnt = dev->pending_chunks_count(j);
                if (cnt > 0) {
                    auto link = dev->get_link(j);
                    std::cerr << "[stuck] dev " << i << " -> " << j << ": "
                              << cnt << " pending, bw=" << link->get_bandwidth()
                              << ", busy=" << link->is_busy() << "\n";
                }
            }
        }
        std::cerr << "=== DRAIN DIAGNOSTIC: unresolved callback entries ===\n";
        const auto& tracker = CommonNetworkApi::get_callback_tracker();
        for (const auto& [key, entry] : tracker.entries()) {
            const auto [tag, src, dst, size, cid] = key;
            std::cerr << "[entry] tag=" << tag << " src=" << src
                      << " dst=" << dst << " size=" << size << " cid=" << cid
                      << " both=" << entry.both_callbacks_registered()
                      << " tx_done=" << entry.is_transmission_finished()
                      << "\n";
        }
        std::cerr << "=== DRAIN DIAGNOSTIC: per-Sys stream state ===\n";
        for (auto* sys : systems) {
            if (sys == nullptr || sys->active_workload == nullptr) {
                continue;
            }
            std::cerr << "[sys " << sys->id
                      << "] ready_list=" << sys->ready_list.size()
                      << " first_phase=" << sys->first_phase_streams
                      << " running=" << sys->total_running_streams
                      << " pending_events=" << sys->pending_events << "\n";
            for (auto& kv : sys->active_Streams) {
                if (!kv.second.empty()) {
                    std::cerr << "    active_Streams[vn " << kv.first << "]:";
                    for (auto* st : kv.second) {
                        std::cerr << " " << st->stream_id << "(ph"
                                  << st->phases_to_go.size() << ")";
                    }
                    std::cerr << "\n";
                }
            }
        }
        std::cerr.flush();
    });
    runtime.run();

    AstraSim::LoggerFactory::shutdown();
    return 0;
}
