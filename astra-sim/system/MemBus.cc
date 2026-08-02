/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/system/MemBus.hh"

#include "astra-sim/system/LogGP.hh"
#include "astra-sim/system/Sys.hh"

using namespace std;
using namespace AstraSim;

MemBus::MemBus(string side1,
               string side2,
               Sys* sys,
               Tick L,
               Tick o,
               Tick g,
               double G,
               bool model_shared_bus,
               int communication_delay,
               bool attach)
    : fast_stat_NPU_to_MA(BusType::Shared, 0, 10, 0, 0),
      fast_stat_MA_to_NPU(BusType::Shared, 0, 10, 0, 0),
      usual_stat_NPU_to_MA(BusType::Shared, 0, communication_delay, 0, 0),
      usual_stat_MA_to_NPU(BusType::Shared, 0, communication_delay, 0, 0) {
    NPU_side = new LogGP(side1, sys, L, o, g, G, EventType::MA_to_NPU);
    MA_side = new LogGP(side2, sys, L, o, g, G, EventType::NPU_to_MA);
    NPU_side->partner = MA_side;
    MA_side->partner = NPU_side;
    this->sys = sys;
    this->model_shared_bus = model_shared_bus;
    this->communication_delay = communication_delay;
    fast_stat_NPU_to_MA.sys_id = sys->id;
    fast_stat_NPU_to_MA.event = EventType::NPU_to_MA;
    fast_stat_MA_to_NPU.sys_id = sys->id;
    fast_stat_MA_to_NPU.event = EventType::MA_to_NPU;
    usual_stat_NPU_to_MA.sys_id = sys->id;
    usual_stat_NPU_to_MA.event = EventType::NPU_to_MA;
    usual_stat_MA_to_NPU.sys_id = sys->id;
    usual_stat_MA_to_NPU.event = EventType::MA_to_NPU;
    if (attach) {
        NPU_side->attach_mem_bus(sys, L, o, g, 0.0038, model_shared_bus,
                                 communication_delay);
    }
}

MemBus::~MemBus() {
    delete NPU_side;
    delete MA_side;
}

void MemBus::send_from_NPU_to_MA(MemBus::Transmition transmition,
                                 int bytes,
                                 bool processed,
                                 bool send_back,
                                 Callable* callable) {
    if (model_shared_bus && transmition == Transmition::Usual) {
        NPU_side->request_read(bytes, processed, send_back, callable);
    } else {
        if (transmition == Transmition::Fast) {
            sys->register_event(callable, EventType::NPU_to_MA,
                                &fast_stat_NPU_to_MA, 10);
        } else {
            sys->register_event(callable, EventType::NPU_to_MA,
                                &usual_stat_NPU_to_MA, communication_delay);
        }
    }
}

void MemBus::send_from_MA_to_NPU(MemBus::Transmition transmition,
                                 int bytes,
                                 bool processed,
                                 bool send_back,
                                 Callable* callable) {
    if (model_shared_bus && transmition == Transmition::Usual) {
        MA_side->request_read(bytes, processed, send_back, callable);
    } else {
        if (transmition == Transmition::Fast) {
            sys->register_event(callable, EventType::MA_to_NPU,
                                &fast_stat_MA_to_NPU, 10);
        } else {
            sys->register_event(callable, EventType::MA_to_NPU,
                                &usual_stat_MA_to_NPU, communication_delay);
        }
    }
}
