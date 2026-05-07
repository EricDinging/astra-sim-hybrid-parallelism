/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/common/Logging.hh"
#include "common/CmdLineParser.hh"
#include "reconfigurable/ReconfigurableNetworkApi.hh"
#include <astra-network-analytical/common/EventQueue.h>
#include <astra-network-analytical/common/NetworkParser.h>
#include <astra-network-analytical/reconfigurable/Helper.h>
#include <remote_memory_backend/analytical/AnalyticalRemoteMemory.hh>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace AstraSim;
using namespace Analytical;
using namespace AstraSimAnalytical;
using namespace AstraSimAnalyticalReconfigurable;
using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

using bw_matrix_t = std::vector<std::vector<Bandwidth>>;
using lt_matrix_t = std::vector<std::vector<Latency>>;

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
std::map<int, std::vector<std::vector<T>>>
parse_schedule_file(const std::string& filename, const std::string& tag) {
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
                    std::cerr
                        << "[Error] (AstraSim/analytical/reconfigurable) "
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
                      << ": LT=" << lt_mat.size()
                      << " BW=" << bw_mat.size() << std::endl;
            std::exit(1);
        }
        for (size_t i = 0; i < lt_mat.size(); ++i) {
            if (lt_mat[i].size() != bw_mat[i].size()) {
                std::cerr
                    << "[Error] (AstraSim/analytical/reconfigurable) "
                    << "LT matrix col count mismatch for topo_id " << topo_id
                    << " row " << i << ": LT=" << lt_mat[i].size()
                    << " BW=" << bw_mat[i].size() << std::endl;
                std::exit(1);
            }
            for (size_t j = 0; j < lt_mat[i].size(); ++j) {
                if (lt_mat[i][j] < 0) {
                    std::cerr
                        << "[Error] (AstraSim/analytical/reconfigurable) "
                        << "Negative latency in topo_id " << topo_id
                        << " at (" << i << "," << j << "): " << lt_mat[i][j]
                        << std::endl;
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
    const auto workload_configuration =
        cmd_line_parser.get<std::string>("workload-configuration");
    const auto comm_group_configuration =
        cmd_line_parser.get<std::string>("comm-group-configuration");
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

    AstraSim::LoggerFactory::init(logging_configuration, logging_folder);

    const auto event_queue = std::make_shared<EventQueue>();
    Topology::set_event_queue(event_queue);

    const auto network_parser = NetworkParser(network_configuration);
    const auto npus_count = network_parser.get_npus_counts_per_dim()[0];
    const Latency scalar_latency = network_parser.get_latencies_per_dim()[0];
    const Latency reconfig_latency = network_parser.get_reconfig_time();

    auto logger = AstraSim::LoggerFactory::get_logger("default");
    logger->debug("Parsing BW schedule from {}", bw_schedule_path);
    auto bw_schedules =
        parse_schedule_file<Bandwidth>(bw_schedule_path, "BW");
    validate_bw_schedules(bw_schedules);

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

    auto tm = std::make_shared<TopologyManager>(
        npus_count, npus_count, event_queue.get(), std::move(bw_schedules),
        std::move(latency_schedules), npus_per_dim, !npus_per_dim.empty());

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
    auto systems = std::vector<Sys*>();

    auto queues_per_dim = std::vector<int>();
    for (auto i = 0; i < 1; i++) {
        queues_per_dim.push_back(num_queues_per_dim);
    }

    for (int i = 0; i < npus_count; i++) {
        // create network and system
        auto network_api = std::make_unique<ReconfigurableNetworkApi>(i);
        auto* const system =
            new Sys(i, workload_configuration, comm_group_configuration,
                    system_configuration, memory_api.get(), network_api.get(),
                    {npus_count}, queues_per_dim, injection_scale, comm_scale,
                    rendezvous_protocol);

        // push back network and system
        network_apis.push_back(std::move(network_api));
        systems.push_back(system);
    }

    // Initiate ASTRA-sim simulation
    for (int i = 0; i < npus_count; i++) {
        systems[i]->workload->fire();
    }

    // run simulation
    while (true) {
        event_queue->proceed();
        if (event_queue->finished()) {
            for (int i = 0; i < npus_count; i++) {
                systems[i]->workload->issue_dep_free_nodes();
            }
        }
        if (event_queue->finished()) {
            break;
        }
    }

    // terminate simulation
    AstraSim::LoggerFactory::shutdown();
    return 0;
}
