/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __RING_HH__
#define __RING_HH__

#include <deque>

#include "astra-sim/system/MemBus.hh"
#include "astra-sim/system/MyPacket.hh"
#include "astra-sim/system/astraccl/Algorithm.hh"
#include "astra-sim/system/astraccl/native_collectives/logical_topology/RingTopology.hh"

namespace AstraSim {

class Ring : public Algorithm {
  public:
    Ring(ComType type,
         int id,
         RingTopology* ring_topology,
         uint64_t data_size,
         RingTopology::Direction direction,
         InjectionPolicy injection_policy);
    virtual void run(EventType event, CallData* data);
    void process_stream_count();
    void release_packets();
    virtual void process_max_count();
    void reduce();
    bool iteratable();
    virtual int get_non_zero_latency_packets();
    void insert_packet(Callable* sender);
    bool ready();
    void exit();

    RingTopology::Direction dimension;
    RingTopology::Direction direction;
    MemBus::Transmition transmition;
    int zero_latency_packets;
    int non_zero_latency_packets;
    int id;
    int curr_receiver;
    int curr_sender;
    int nodes_in_ring;
    int stream_count;
    int max_count;
    int remained_packets_per_max_count;
    int remained_packets_per_message;
    int parallel_reduce;
    InjectionPolicy injection_policy;
    // deque, not list: push_back/pop_front only, and it chunk-allocates
    // instead of one node per packet. Element references (locked_packets holds
    // &packets.back()) stay valid across push_back/pop_front at the ends.
    std::deque<MyPacket> packets;
    bool toggle;
    long free_packets;
    long total_packets_sent;
    long total_packets_received;
    uint64_t msg_size;
    std::list<MyPacket*> locked_packets;
    bool processed;
    bool send_back;
    bool NPU_to_MA;
};

}  // namespace AstraSim

#endif /* __RING_HH__ */
