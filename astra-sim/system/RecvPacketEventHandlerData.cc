/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/system/RecvPacketEventHandlerData.hh"

#include <vector>

#include "astra-sim/system/WorkloadLayerHandlerData.hh"

using namespace AstraSim;

namespace {
// Freelist of released handler-data objects (concrete type only; the
// Sys::handleEvent PacketReceived branch is the sole delete site and only
// ever sees this class). Never shrinks; single-threaded.
std::vector<RecvPacketEventHandlerData*> recv_ehd_pool;
}  // namespace

RecvPacketEventHandlerData::RecvPacketEventHandlerData() {
    this->workload = nullptr;
    this->wlhd = nullptr;
    this->owner = nullptr;
    this->custom_algorithm = nullptr;
}

RecvPacketEventHandlerData::RecvPacketEventHandlerData(
    BaseStream* owner, int sys_id, EventType event, int vnet, int stream_id)
    : BasicEventHandlerData(sys_id, event) {
    this->workload = nullptr;
    this->wlhd = nullptr;
    this->owner = owner;
    this->custom_algorithm = nullptr;
    this->vnet = vnet;
    this->stream_id = stream_id;
    this->message_end = true;
    ready_time = Sys::boostedTick();
}

RecvPacketEventHandlerData* RecvPacketEventHandlerData::acquire(
    BaseStream* owner, int sys_id, EventType event, int vnet, int stream_id) {
    if (recv_ehd_pool.empty()) {
        return new RecvPacketEventHandlerData(owner, sys_id, event, vnet,
                                              stream_id);
    }
    RecvPacketEventHandlerData* data = recv_ehd_pool.back();
    recv_ehd_pool.pop_back();
    // Same assignments, same order as the 5-arg constructor: base-class
    // fields first (the constructor delegates to BasicEventHandlerData),
    // then the members, ending with the single boostedTick() read the
    // constructor also did exactly once.
    data->sys_id = sys_id;
    data->event = event;
    data->workload = nullptr;
    data->wlhd = nullptr;
    data->owner = owner;
    data->custom_algorithm = nullptr;
    data->vnet = vnet;
    data->stream_id = stream_id;
    data->message_end = true;
    data->ready_time = Sys::boostedTick();
    return data;
}

void RecvPacketEventHandlerData::release(RecvPacketEventHandlerData* data) {
    recv_ehd_pool.push_back(data);
}
