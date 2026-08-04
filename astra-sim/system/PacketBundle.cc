/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/system/PacketBundle.hh"

#include <vector>

using namespace AstraSim;

namespace {
// Freelist of released bundles. Never shrinks; the process is single-threaded
// and exits with the pool (no per-object delete, same as the old delete-this
// lifetime ending at process exit).
std::vector<PacketBundle*> bundle_pool;
}  // namespace

PacketBundle::PacketBundle(Sys* sys,
                           BaseStream* stream,
                           MyPacket* locked_packet,
                           bool needs_processing,
                           bool send_back,
                           uint64_t size,
                           MemBus::Transmition transmition) {
    this->sys = sys;
    this->locked_packet = locked_packet;
    this->needs_processing = needs_processing;
    this->send_back = send_back;
    this->size = size;
    this->stream = stream;
    this->transmition = transmition;
    creation_time = Sys::boostedTick();
}

PacketBundle::PacketBundle(Sys* sys,
                           BaseStream* stream,
                           bool needs_processing,
                           bool send_back,
                           uint64_t size,
                           MemBus::Transmition transmition) {
    this->sys = sys;
    this->locked_packet = nullptr;
    this->needs_processing = needs_processing;
    this->send_back = send_back;
    this->size = size;
    this->stream = stream;
    this->transmition = transmition;
    creation_time = Sys::boostedTick();
}

PacketBundle* PacketBundle::acquire(Sys* sys,
                                    BaseStream* stream,
                                    MyPacket* locked_packet,
                                    bool needs_processing,
                                    bool send_back,
                                    uint64_t size,
                                    MemBus::Transmition transmition) {
    if (bundle_pool.empty()) {
        return new PacketBundle(sys, stream, locked_packet, needs_processing,
                                send_back, size, transmition);
    }
    PacketBundle* bundle = bundle_pool.back();
    bundle_pool.pop_back();
    // Same assignments, same order as the matching constructor (including the
    // single boostedTick() read, which the constructor also did exactly once).
    bundle->sys = sys;
    bundle->locked_packet = locked_packet;
    bundle->needs_processing = needs_processing;
    bundle->send_back = send_back;
    bundle->size = size;
    bundle->stream = stream;
    bundle->transmition = transmition;
    bundle->creation_time = Sys::boostedTick();
    return bundle;
}

PacketBundle* PacketBundle::acquire(Sys* sys,
                                    BaseStream* stream,
                                    bool needs_processing,
                                    bool send_back,
                                    uint64_t size,
                                    MemBus::Transmition transmition) {
    return acquire(sys, stream, nullptr, needs_processing, send_back, size,
                   transmition);
}

void PacketBundle::release(PacketBundle* bundle) {
    bundle_pool.push_back(bundle);
}

void PacketBundle::send_to_MA() {
    sys->memBus->send_from_NPU_to_MA(transmition, size, needs_processing,
                                     send_back, this);
}

void PacketBundle::send_to_NPU() {
    sys->memBus->send_from_MA_to_NPU(transmition, size, needs_processing,
                                     send_back, this);
}

void PacketBundle::call(EventType event, CallData* data) {
    if (needs_processing == true) {
        needs_processing = false;
        // this->delay[ns], size[bytes] local_mem_bw[bytes/s]
        this->delay = static_cast<uint64_t>(static_cast<double>(size) /
                                            sys->local_mem_bw * 1e9)  // write
                      + static_cast<uint64_t>(static_cast<double>(size) /
                                              sys->local_mem_bw * 1e9)  // read
                      + static_cast<uint64_t>(static_cast<double>(size) /
                                              sys->local_mem_bw * 1e9);  // read
        sys->try_register_event(this, EventType::CommProcessingFinished, data,
                                this->delay);
        return;
    }
    Tick current = Sys::boostedTick();
    if (locked_packet != nullptr) {
        locked_packet->ready_time = current;
    }
    stream->call(EventType::General, data);
    release(this);
}
