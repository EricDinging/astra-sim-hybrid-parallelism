/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "reconfigurable/ReconfigurableNetworkApi.hh"
#include <astra-network-analytical/reconfigurable/Chunk.h>
#include <cassert>

using namespace AstraSim;
using namespace AstraSimAnalyticalReconfigurable;
using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

std::shared_ptr<TopologyManager> ReconfigurableNetworkApi::tm;

void ReconfigurableNetworkApi::set_topology(
    std::shared_ptr<TopologyManager> tm_ptr) noexcept {
    assert(tm_ptr != nullptr);

    // move topology
    ReconfigurableNetworkApi::tm = std::move(tm_ptr);
}

ReconfigurableNetworkApi::ReconfigurableNetworkApi(const int rank) noexcept
    : CommonNetworkApi(rank) {
    assert(rank >= 0);
}

bool ReconfigurableNetworkApi::sim_reconfig(int topo_id) {
    return tm->reconfigure(topo_id);
}

void ReconfigurableNetworkApi::increment_inflight_coll() {
    tm->inflight_coll++;
}

void ReconfigurableNetworkApi::decrement_inflight_coll() {
    tm->inflight_coll--;
}

int ReconfigurableNetworkApi::get_inflight_coll() {
    return tm->inflight_coll;
}

int ReconfigurableNetworkApi::sim_send(void* const buffer,
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
        ReconfigurableNetworkApi::chunk_id_generator.create_send_chunk_id(
            tag, src, dst, count);

    // register the send callback (single probe: the entry is created here if
    // the recv operation hasn't been issued yet)
    auto* const entry =
        &(callback_tracker.find_or_create_entry(tag, src, dst, count, chunk_id)
              .first->second);
    entry->set_generator_entry(gen_entry);
    entry->register_send_callback(msg_handler, fun_arg);

    // initiate transmission from src -> dst on the cached route (no
    // throwaway 2-node stub route); the arg carries the tracker-entry
    // pointer so arrival needs no hash lookup
    const auto arg_ptr = static_cast<void*>(
        new ChunkArrivalArg{entry, tag, src, dst, count, chunk_id});
    tm->send(count, src, dst, ReconfigurableNetworkApi::process_chunk_arrival,
             arg_ptr);

    // return
    return 0;
}
