#ifndef SCHEDULER_HH
#define SCHEDULER_HH

#include <iostream>

#include "astra-sim/system/Callable.hh"

#include "astra-sim/system/CommunicatorGroup.hh"
#include "astra-sim/system/Sys.hh"


namespace AstraSim {

class Scheduler {
private:
    AstraSim::Sys* sys;

public:
    Scheduler(){};
    Scheduler(AstraSim::Sys* system) : sys(system) {};

    bool pre_reconfig(int cur_comm_group_id, int prev_comm_group_id);
    bool post_reconfig(int cur_comm_group_id);
};
}

#endif // SCHEDULER_HH