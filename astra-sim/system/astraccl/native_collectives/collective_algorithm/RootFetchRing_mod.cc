/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/system/astraccl/native_collectives/collective_algorithm/RootFetchRing.hh"

using namespace AstraSim;

int AstraSim::g_root_fetch_mf = 2;

RootFetchRing::RootFetchRing(ComType type,
                             int id,
                             RingTopology* ring_topology,
                             uint64_t data_size,
                             RingTopology::Direction direction,
                             InjectionPolicy injection_policy)
    : Ring(type,
           id,
           ring_topology,
           data_size,
           direction,
           injection_policy) {
    /*
     * First integration point for the root-fetch idea:
     *   - keep the native ring mechanics unchanged
     *   - inflate the per-rank stream length so the aggregate number of sends
     *     matches the derived root-fetch message-count model
     *
     * This is intentionally a message-count surrogate, not yet a protocol-
     * accurate request/response implementation.
     */
    if (type != ComType::Reduce_Scatter) {
        return;
    }

    if (g_root_fetch_mf != 1 && g_root_fetch_mf != 2 && g_root_fetch_mf != 4) {
        Sys::sys_panic(
            "RootFetchRing only supports hard-coded m_f values in {1, 2, 4}");
    }

    name = Name::RootFetchRing;
    extra_fetch_messages_count = compute_extra_fetch_messages(ring_topology);
    stream_count += extra_fetch_messages_count;
    max_count += extra_fetch_messages_count;
}

int RootFetchRing::get_non_zero_latency_packets() {
    return (nodes_in_ring - 1) + extra_fetch_messages_count;
}

int RootFetchRing::compute_extra_fetch_messages(RingTopology* ring_topology) const {
    const int nodes = ring_topology->get_nodes_in_ring();
    const int total_failures = nodes / 4;
    // Use total_failures (the worst-case / max across all nodes) so every node
    // in the ring inflates stream_count by the same amount.  Per-node values
    // are asymmetric and cause the ring to deadlock because some nodes finish
    // early while others keep waiting for receives that never arrive.
    return (g_root_fetch_mf - 1) * total_failures;
}

