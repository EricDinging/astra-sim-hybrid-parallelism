#include "astra-sim/workload/Scheduler.hh"

// #include "astra-sim/common/Logging.hh"
// #include "astra-sim/system/IntData.hh"
// #include "astra-sim/system/MemEventHandlerData.hh"
// #include "astra-sim/system/RecvPacketEventHandlerData.hh"
// #include "astra-sim/system/SendPacketEventHandlerData.hh"
// #include "astra-sim/system/WorkloadLayerHandlerData.hh"
// #include <json/json.hpp>

#include <iostream>
#include <stdlib.h>
#include <unistd.h>

using namespace std;

bool AstraSim::Scheduler::pre_reconfig(int cur_comm_group_id, int prev_comm_group_id) {
    // if (prev_comm_group_id != cur_comm_group_id || prev_comm_group_id == -1) {
    //     int pg_id = cur_comm_group_id;
    //     int topo_id = 0;

    //     if (pg_id == 3 || pg_id == 4) {
    //         topo_id = 1;
    //     }

    //     bool can_config = sys->comm_NI->sim_reconfig(topo_id);
    //     if (!can_config) {
    //         return false;
    //     }

    //     std::cout << "RANK: " << this->sys->id 
    //                 << " Switching to comm group: " << cur_comm_group_id 
    //                 << std::endl;

    //     previous_group_id = cur_comm_group_id;
    // }

    if(prev_comm_group_id != cur_comm_group_id) {
        // TODO use suitable topo_id

        int topo_id = 0;
        if (prev_comm_group_id == -1) {
            topo_id = 0; // default topology
        } else if (prev_comm_group_id >= 0 && cur_comm_group_id == -1) {
            topo_id = 1;
        }
        
        bool can_config = this->sys->comm_NI->sim_reconfig(topo_id);
        if (!can_config) {
            std::cout << "RANK: " << this->sys->id << " Switching to comm group failed: " << cur_comm_group_id << std::endl;
            return false;
        }
        
        std::cout << "RANK: " << this->sys->id << " Switching to comm group: " << cur_comm_group_id << std::endl;
    }

    return true;
}

bool AstraSim::Scheduler::post_reconfig(int cur_comm_group_id) {
    return true;
}