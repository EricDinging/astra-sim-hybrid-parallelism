/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __PACKET_BUNDLE_HH__
#define __PACKET_BUNDLE_HH__

#include "astra-sim/system/BaseStream.hh"
#include "astra-sim/system/Callable.hh"
#include "astra-sim/system/Common.hh"
#include "astra-sim/system/MemBus.hh"
#include "astra-sim/system/MyPacket.hh"

namespace AstraSim {

class Sys;
class PacketBundle : public Callable {
  public:
    PacketBundle(Sys* sys,
                 BaseStream* stream,
                 MyPacket* locked_packet,
                 bool needs_processing,
                 bool send_back,
                 uint64_t size,
                 MemBus::Transmition transmition);
    PacketBundle(Sys* sys,
                 BaseStream* stream,
                 bool needs_processing,
                 bool send_back,
                 uint64_t size,
                 MemBus::Transmition transmition);
    // Freelist: bundles are allocated once per packet and freed at the end of
    // call(); acquire() reuses a released bundle (reset assigns exactly what
    // the matching constructor assigns, in the same order) instead of hitting
    // the allocator every packet. Single-threaded, like the rest of the
    // system layer.
    static PacketBundle* acquire(Sys* sys,
                                 BaseStream* stream,
                                 MyPacket* locked_packet,
                                 bool needs_processing,
                                 bool send_back,
                                 uint64_t size,
                                 MemBus::Transmition transmition);
    static PacketBundle* acquire(Sys* sys,
                                 BaseStream* stream,
                                 bool needs_processing,
                                 bool send_back,
                                 uint64_t size,
                                 MemBus::Transmition transmition);
    static void release(PacketBundle* bundle);
    void send_to_MA();
    void send_to_NPU();
    void call(EventType event, CallData* data);

    Sys* sys;
    // Single pointer, not a container: the collectives release exactly one
    // packet per bundle (remained_packets_per_max_count == 1, asserted at the
    // release sites); nullptr for the processing-only constructor.
    MyPacket* locked_packet;
    bool needs_processing;
    bool send_back;
    uint64_t size;
    BaseStream* stream;
    MemBus::Transmition transmition;
    Tick delay;
    Tick creation_time;
};

}  // namespace AstraSim

#endif /* __PACKET_BUNDLE_HH__ */
