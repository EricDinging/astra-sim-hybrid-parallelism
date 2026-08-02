/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/system/StreamBaseline.hh"

#include "astra-sim/system/astraccl/Algorithm.hh"

using namespace AstraSim;

StreamBaseline::StreamBaseline(Sys* owner,
                               DataSet* dataset,
                               int stream_id,
                               std::list<CollectivePhase> phases_to_go,
                               int priority)
    : BaseStream(stream_id, owner, std::move(phases_to_go)) {
    this->owner = owner;
    this->stream_id = stream_id;
    this->dataset = dataset;
    this->priority = priority;
    steps_finished = 0;
    initial_data_size = this->phases_to_go.front().initial_data_size;
}

void StreamBaseline::init() {
    initialized = true;
    last_init = Sys::boostedTick();
    if (!my_current_phase.enabled) {
        return;
    }
    my_current_phase.algorithm->run(EventType::StreamInit, nullptr);
    if (steps_finished == 1) {
        queuing_delay.push_back(last_phase_change - creation_time);
    }
    queuing_delay.push_back(Sys::boostedTick() - last_phase_change);
    total_packets_sent = 1;
}

void StreamBaseline::call(EventType event, CallData* data) {
    // `data` is always non-null here: the only caller is PacketBundle::call,
    // which forwards one of MemBus's preallocated per-variant SharedBusStat
    // payloads. Those are owned by MemBus and only read here, so there is
    // nothing to free (the old per-packet heap allocation and its delete,
    // plus the vestigial null check, are gone).
    SharedBusStat* sharedBusStat = (SharedBusStat*)data;
    update_bus_stats(BusType::Both, sharedBusStat);
    my_current_phase.algorithm->run(EventType::General, data);
}

void StreamBaseline::consume(RecvPacketEventHandlerData* message) {
    net_message_latency.back() +=
        Sys::boostedTick() - message->ready_time;  // not accurate
    net_message_counter++;
    my_current_phase.algorithm->run(EventType::PacketReceived, message);
}
