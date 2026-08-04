/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "congestion_unaware/CongestionUnawareNetworkApi.hh"
#include <cassert>

using namespace AstraSim;
using namespace AstraSimAnalyticalCongestionUnaware;
using namespace NetworkAnalytical;
using namespace NetworkAnalyticalCongestionUnaware;

std::shared_ptr<Topology> CongestionUnawareNetworkApi::topology;

void CongestionUnawareNetworkApi::set_topology(
    std::shared_ptr<Topology> topology_ptr) noexcept {
    assert(topology_ptr != nullptr);

    // move topology
    CongestionUnawareNetworkApi::topology = std::move(topology_ptr);

    // set topology-related values
    CongestionUnawareNetworkApi::dims_count =
        CongestionUnawareNetworkApi::topology->get_dims_count();
    CongestionUnawareNetworkApi::bandwidth_per_dim =
        CongestionUnawareNetworkApi::topology->get_bandwidth_per_dim();
}

CongestionUnawareNetworkApi::CongestionUnawareNetworkApi(
    const int rank) noexcept
    : CommonNetworkApi(rank) {
    assert(rank >= 0);
}

int CongestionUnawareNetworkApi::sim_send(void* const buffer,
                                          const uint64_t count,
                                          const int type,
                                          const int dst,
                                          const int tag,
                                          sim_request* const request,
                                          void (*msg_handler)(void*),
                                          void* const fun_arg) {
    // query chunk id (and the generator entry, for probe-free retire)
    const auto src = sim_comm_get_rank();
    const auto [chunk_id, gen_entry] =
        CongestionUnawareNetworkApi::chunk_id_generator.create_send_chunk_id(
            tag, src, dst, count);

    // register the send callback (single probe: the entry is created here if
    // the recv operation hasn't been issued yet)
    auto* const entry =
        &(callback_tracker.find_or_create_entry(tag, src, dst, count, chunk_id)
              .first->second);
    entry->set_generator_entry(gen_entry);
    entry->register_send_callback(msg_handler, fun_arg);

    // create chunk; the arg carries the tracker-entry pointer so arrival
    // needs no hash lookup
    const auto arg_ptr = static_cast<void*>(
        new ChunkArrivalArg{entry, tag, src, dst, count, chunk_id});

    // compute send communication delay (in AstraSim format)
    const auto send_delay_ns = topology->send(src, dst, count);
    const auto send_delay = static_cast<double>(send_delay_ns);
    const auto delta = timespec_t({NS, send_delay});

    // register chunk arrival event after send communication delay
    sim_schedule(delta, CongestionUnawareNetworkApi::process_chunk_arrival,
                 arg_ptr);

    // return
    return 0;
}
