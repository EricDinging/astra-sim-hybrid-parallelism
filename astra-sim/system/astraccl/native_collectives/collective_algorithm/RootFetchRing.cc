/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/system/astraccl/native_collectives/collective_algorithm/RootFetchRing.hh"

using namespace AstraSim;

int AstraSim::g_root_fetch_mf = 2;
RootFetchRing::QuarterPlacement AstraSim::g_root_fetch_quarter =
    RootFetchRing::QuarterPlacement::Q1;

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
    const int extra_fetch_messages = compute_extra_fetch_messages(ring_topology);
    stream_count += extra_fetch_messages;
    max_count += extra_fetch_messages;
}

int RootFetchRing::compute_extra_fetch_messages(RingTopology* ring_topology) const {
    const int nodes = ring_topology->get_nodes_in_ring();
    const int total_failures = nodes / 4;
    const int index_in_ring = ring_topology->get_index_in_ring();

    int affected_failures = 0;
    for (int failure_index = 1; failure_index <= total_failures; ++failure_index) {
        const int failure_round = failure_round_for(failure_index);
        if (index_in_ring < (nodes - failure_round)) {
            affected_failures++;
        }
    }

    return (g_root_fetch_mf - 1) * affected_failures;
}

int RootFetchRing::failure_round_for(int failure_index) const {
    switch (g_root_fetch_quarter) {
    case QuarterPlacement::Q1:
        return failure_index;
    case QuarterPlacement::Q2:
        return (nodes_in_ring / 4) + failure_index;
    case QuarterPlacement::Q3:
        return (nodes_in_ring / 2) + failure_index;
    }
    return failure_index;
}
