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
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <tuple>
#include <string>

using namespace AstraSim;
using namespace Analytical;
using namespace AstraSimAnalytical;
using namespace AstraSimAnalyticalReconfigurable;
using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

using bw_matrix_t = std::vector<std::vector<Bandwidth>>;
using lt_matrix_t = std::vector<std::vector<Latency>>;

static inline std::string trim(const std::string &s) {
    auto ws = " \t\r\n";
    size_t start = s.find_first_not_of(ws);
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

std::map<int, bw_matrix_t> bw_matrix_map;

void parse_bw_matrix(const std::string& filename) {
    std::ifstream file(filename);
    std::string line;

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line.rfind("//", 0) == 0)
            continue;

        if (line.substr(0, 2) == "BW") {
            int topo_id = std::stoi(line.substr(3));
            bw_matrix_t bw_matrix;

            while (std::getline(file, line)) {
                line = trim(line);
                if (line.empty() || line.rfind("//", 0) == 0)
                    continue;

                if (line == "END") {
                    break;
                }

                std::istringstream ss(line);
                Bandwidth value;
                std::vector<Bandwidth> row;
                while (ss >> value) {
                    row.push_back(value);
                }
                bw_matrix.push_back(row);
            }
            bw_matrix_map[topo_id] = bw_matrix;
            std::cout << "Parsed BW matrix for topology: " << topo_id << std::endl;
            for(auto row:bw_matrix){
                for(auto bw: row){
                    std::cout << bw << " ";
                }
                std::cout << std::endl;
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
    const auto circuit_schedules = 
        cmd_line_parser.get<std::string>("circuit-schedules");

    const auto logging_configuration =
        cmd_line_parser.get<std::string>("logging-configuration");
    const auto num_queues_per_dim =
        cmd_line_parser.get<int>("num-queues-per-dim");
    const auto comm_scale = cmd_line_parser.get<double>("comm-scale");
    const auto injection_scale = cmd_line_parser.get<double>("injection-scale");
    const auto rendezvous_protocol =
        cmd_line_parser.get<bool>("rendezvous-protocol");

    AstraSim::LoggerFactory::init(logging_configuration);

    // Instantiate event queue
    const auto event_queue = std::make_shared<EventQueue>();

    Topology::set_event_queue(event_queue);
    std::shared_ptr<TopologyManager> tm;
    // Instantiate TopologyManager

    // Generate topology
    const auto network_parser = NetworkParser(network_configuration);
    const auto npus_count = network_parser.get_npus_counts_per_dim()[0];
    // const auto topology = construct_topology(network_parser);

    Latency link_latency = network_parser.get_latencies_per_dim()[0];
    Latency reconfig_latency = network_parser.get_reconfig_time();

    lt_matrix_t lt_matrix;
    lt_matrix.resize(npus_count, std::vector<Latency>(npus_count, link_latency));
   

    // Get topology information
    std::cout << "Parsing BW Matrix..." << std::endl;
    parse_bw_matrix(circuit_schedules);
    std::cout << "BW Matrix parsed" << std::endl;

    tm = std::make_shared<TopologyManager>(npus_count, npus_count, event_queue.get(), bw_matrix_map);

    // Initialize the topology to the first topology in the map
    tm->reconfigure(bw_matrix_map[0], lt_matrix, 0, 0);

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
                    {npus_count}, queues_per_dim, injection_scale,
                    comm_scale, rendezvous_protocol);

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
        if(event_queue->finished()){
            for (int i = 0; i < npus_count; i++) {
                systems[i]->workload->issue_dep_free_nodes();
            }
        }
        if(event_queue->finished())
            break;
    }

    // terminate simulation
    AstraSim::LoggerFactory::shutdown();
    return 0;
}
