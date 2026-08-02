/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __MEM_BUS_HH__
#define __MEM_BUS_HH__

#include <string>

#include "astra-sim/system/Callable.hh"
#include "astra-sim/system/Common.hh"
#include "astra-sim/system/SharedBusStat.hh"

namespace AstraSim {

class Sys;
class LogGP;
class MemBus {
  public:
    enum class Transmition { Fast, Usual };

    MemBus(std::string side1,
           std::string side2,
           Sys* sys,
           Tick L,
           Tick o,
           Tick g,
           double G,
           bool model_shared_bus,
           int communication_delay,
           bool attach);
    ~MemBus();

    void send_from_NPU_to_MA(Transmition transmition,
                             int bytes,
                             bool processed,
                             bool send_back,
                             Callable* callable);
    void send_from_MA_to_NPU(Transmition transmition,
                             int bytes,
                             bool processed,
                             bool send_back,
                             Callable* callable);

    LogGP* NPU_side;
    LogGP* MA_side;
    Sys* sys;
    int communication_delay;
    bool model_shared_bus;

    // Preallocated stat payloads for the non-shared-bus path: every send used
    // to heap-allocate a SharedBusStat whose contents are run-constant (delay
    // 10 for Fast, communication_delay for Usual; direction in `event`).
    // Consumers (PacketBundle::call -> StreamBaseline::call) only read them,
    // so one immutable instance per (latency, direction) variant suffices and
    // nothing is freed downstream.
    SharedBusStat fast_stat_NPU_to_MA;
    SharedBusStat fast_stat_MA_to_NPU;
    SharedBusStat usual_stat_NPU_to_MA;
    SharedBusStat usual_stat_MA_to_NPU;
};

}  // namespace AstraSim

#endif /* __MEM_BUS_HH__ */
