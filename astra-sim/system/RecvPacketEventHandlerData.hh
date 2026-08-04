/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __RECV_PACKET_EVENT_HANDLER_DATA_HH__
#define __RECV_PACKET_EVENT_HANDLER_DATA_HH__

#include "astra-sim/system/BaseStream.hh"
#include "astra-sim/system/BasicEventHandlerData.hh"
#include "astra-sim/system/astraccl/custom_collectives/CustomAlgorithm.hh"

namespace AstraSim {

class WorkloadLayerHandlerData;

class RecvPacketEventHandlerData : public BasicEventHandlerData {
  public:
    RecvPacketEventHandlerData();
    RecvPacketEventHandlerData(BaseStream* owner,
                               int sys_id,
                               EventType event,
                               int vnet,
                               int stream_id);
    // Freelist for the per-packet 5-arg construction path: one of these is
    // new'd per received packet and deleted in Sys::handleEvent. acquire()
    // reuses a released object (reset assigns every field the 5-arg
    // constructor assigns, in the same order). The default-constructed sites
    // (Workload / CustomAlgorithm recv nodes) stay on plain new but release
    // into the same pool; acquire fully reinitializes either way.
    // Single-threaded.
    static RecvPacketEventHandlerData* acquire(BaseStream* owner,
                                               int sys_id,
                                               EventType event,
                                               int vnet,
                                               int stream_id);
    static void release(RecvPacketEventHandlerData* data);

    Workload* workload;
    WorkloadLayerHandlerData* wlhd;
    BaseStream* owner;
    CustomAlgorithm* custom_algorithm;
    int vnet;
    int stream_id;
    bool message_end;
    Tick ready_time;
};

}  // namespace AstraSim

#endif /* __RECV_PACKET_EVENT_HANDLER_DATA_HH__ */
