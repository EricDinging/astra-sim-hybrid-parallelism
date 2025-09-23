#ifndef SCHEDULER_HH
#define SCHEDULER_HH

#include <iostream>
#include <vector>
#include <unordered_map>

#include "astra-sim/system/Callable.hh"

#include "astra-sim/system/CommunicatorGroup.hh"
#include "astra-sim/system/Sys.hh"
#include <yaml-cpp/yaml.h>
#include <fstream>


namespace AstraSim {

class Scheduler {
private:
    AstraSim::Sys* sys;
    std::unordered_map<int, std::vector<int>> comm_group_to_start_indexes;

    int cur_comm_idx = 0;
    bool reconfigure(int cur_comm_group_id, int prev_comm_group_id);

public:
    Scheduler(){};
    Scheduler(AstraSim::Sys* system);
    bool pre_reconfig(int cur_comm_group_id, int prev_comm_group_id);
    bool post_reconfig(int cur_comm_group_id);
};
}

#endif // SCHEDULER_HH