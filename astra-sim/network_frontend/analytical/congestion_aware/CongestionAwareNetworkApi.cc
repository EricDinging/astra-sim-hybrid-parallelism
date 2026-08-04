/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "congestion_aware/CongestionAwareNetworkApi.hh"
#include <astra-network-analytical/congestion_aware/Chunk.h>
#include <cassert>

using namespace AstraSim;
using namespace AstraSimAnalyticalCongestionAware;
using namespace NetworkAnalytical;
using namespace NetworkAnalyticalCongestionAware;

std::shared_ptr<Topology> CongestionAwareNetworkApi::topology;

void CongestionAwareNetworkApi::set_topology(
    std::shared_ptr<Topology> topology_ptr) noexcept {
    assert(topology_ptr != nullptr);

    // move topology
    CongestionAwareNetworkApi::topology = std::move(topology_ptr);

    // set topology-related values
    CongestionAwareNetworkApi::dims_count =
        CongestionAwareNetworkApi::topology->get_dims_count();
    CongestionAwareNetworkApi::bandwidth_per_dim =
        CongestionAwareNetworkApi::topology->get_bandwidth_per_dim();
}

CongestionAwareNetworkApi::CongestionAwareNetworkApi(const int rank) noexcept
    : CommonNetworkApi(rank) {
    assert(rank >= 0);
}

int CongestionAwareNetworkApi::sim_send(void* const buffer,
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
        CongestionAwareNetworkApi::chunk_id_generator.create_send_chunk_id(
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
    const auto route = topology->route(src, dst);
    auto chunk = std::make_unique<Chunk>(
        count, route, CongestionAwareNetworkApi::process_chunk_arrival,
        arg_ptr);

    // initiate transmission from src -> dst.
    topology->send(std::move(chunk));

    // return
    return 0;
}
