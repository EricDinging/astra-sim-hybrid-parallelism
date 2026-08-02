/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

// TODO: HardwareResource.cc should be moved to the system layer.

#include "astra-sim/workload/HardwareResource.hh"

using namespace std;
using namespace AstraSim;
using namespace Chakra;

typedef ChakraProtoMsg::NodeType ChakraNodeType;

HardwareResource::HardwareResource(uint32_t num_npus, int sys_id)
    : num_npus(num_npus),
      num_in_flight_cpu_ops(0),
      num_in_flight_gpu_comm_ops(0),
      num_in_flight_ungrouped_comm_ops(0),
      num_in_flight_gpu_comp_ops(0),
      sys_id(sys_id) {

    num_cpu_ops = 0;
    num_gpu_ops = 0;
    num_gpu_comms = 0;

    tics_cpu_ops = 0;
    tics_gpu_ops = 0;
    tics_gpu_comms = 0;

    // cpu_ops_node = NULL;
    // gpu_ops_node = NULL;
    // gpu_comms_node = NULL;
}

void HardwareResource::occupy(
    const shared_ptr<Chakra::FeederV3::ETFeederNode>& node) {
    if (node->is_cpu_op()) {
        assert(num_in_flight_cpu_ops == 0);
        ++num_in_flight_cpu_ops;
        ++num_cpu_ops;
        cpu_ops_node.emplace(node->id());
    } else {
        if (node->type() == ChakraNodeType::COMP_NODE) {
            assert(num_in_flight_gpu_comp_ops == 0);
            ++num_in_flight_gpu_comp_ops;
            ++num_gpu_ops;
            // gpu_ops_node = node;
            gpu_ops_node.emplace(node->id());
        } else {
            if (node->type() == ChakraNodeType::COMM_RECV_NODE) {
                return;
            }
            ++num_in_flight_gpu_comm_ops;
            const int cg_id = comm_group_of(node);
            if (cg_id >= 0) {
                ++num_in_flight_comm_per_group[cg_id];
            } else {
                ++num_in_flight_ungrouped_comm_ops;
            }
            ++num_gpu_comms;
            // gpu_comms_node = node;
            gpu_comms_node.emplace(node->id());
        }
    }
}

void HardwareResource::release(
    const shared_ptr<Chakra::FeederV3::ETFeederNode>& node) {
    if (node->is_cpu_op()) {
        --num_in_flight_cpu_ops;
        assert(num_in_flight_cpu_ops == 0);
        this->cpu_ops_node.erase(node->id());
    } else {
        if (node->type() == ChakraNodeType::COMP_NODE) {
            --num_in_flight_gpu_comp_ops;
            assert(num_in_flight_gpu_comp_ops == 0);
            this->gpu_ops_node.erase(node->id());
        } else {
            if (node->type() == ChakraNodeType::COMM_RECV_NODE) {
                return;
            }
            --num_in_flight_gpu_comm_ops;
            const int cg_id = comm_group_of(node);
            if (cg_id >= 0) {
                auto it = num_in_flight_comm_per_group.find(cg_id);
                assert(it != num_in_flight_comm_per_group.end() &&
                       it->second > 0);
                --it->second;
            } else {
                assert(num_in_flight_ungrouped_comm_ops > 0);
                --num_in_flight_ungrouped_comm_ops;
            }
            this->gpu_comms_node.erase(node->id());
        }
    }
}

bool HardwareResource::is_available(
    const shared_ptr<Chakra::FeederV3::ETFeederNode>& node) const {
    if (node->is_cpu_op()) {
        if (num_in_flight_cpu_ops == 0) {
            return true;
        } else {
            return false;
        }
    } else {
        if (node->type() == ChakraNodeType::COMP_NODE) {
            if (num_in_flight_gpu_comp_ops == 0) {
                return true;
            } else {
                return false;
            }
        } else {
            // RECV nodes always allowed (they don't occupy the comm slot).
            // Other comm nodes are bounded to avoid overwhelming the network
            // event queue and to bound how far a fast rank can get ahead of
            // its peers: grouped collectives by a per-comm-group window
            // (order-of-admission is enforced separately by Workload),
            // ungrouped comm ops (sends, pg-less collectives) by a shared cap.
            if (node->type() == ChakraNodeType::COMM_RECV_NODE) {
                return true;
            }
            if (comm_cap_bypass) {
                return true;
            }
            const int cg_id = comm_group_of(node);
            if (cg_id >= 0) {
                auto it = num_in_flight_comm_per_group.find(cg_id);
                return it == num_in_flight_comm_per_group.end() ||
                       it->second < kMaxInflightCommPerGroup;
            }
            return num_in_flight_ungrouped_comm_ops < kMaxInflightGpuCommOps;
        }
    }
}

int HardwareResource::comm_group_of(
    const shared_ptr<Chakra::FeederV3::ETFeederNode>& node) const {
    if (node->type() != ChakraNodeType::COMM_COLL_NODE) {
        return -1;
    }
    // Fast path: the Workload's Kahn scan already parsed every grouped
    // COMM_COLL node, so a miss means "ungrouped" -- no attribute fetch
    // through the (global, mutex-guarded) feeder cache per occupancy check.
    if (node_cg_map_ != nullptr) {
        const auto it = node_cg_map_->find(node->id());
        return it != node_cg_map_->end() ? it->second : -1;
    }
    const std::string pg = node->pg_name<std::string>("");
    if (pg.empty()) {
        return -1;
    }
    try {
        return std::stoi(pg);
    } catch (const std::exception&) {
        return -1;
    }
}

void HardwareResource::report() {
    cout << "num_cpu_ops: " << num_cpu_ops << endl;
    cout << "num_gpu_ops: " << num_gpu_ops << endl;
    cout << "num_gpu_comms: " << num_gpu_comms << endl;

    cout << "tics_cpu_ops: " << tics_cpu_ops << endl;
    cout << "tics_gpu_ops: " << tics_gpu_ops << endl;
    cout << "tics_gpu_comms: " << tics_gpu_comms << endl;
}
