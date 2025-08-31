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

void ReconfigurableNetworkApi::sim_reconfig(int topo_id) {
    tm->reconfigure(topo_id);
}

int ReconfigurableNetworkApi::sim_send(void* const buffer,
                                        const uint64_t count,
                                        const int type,
                                        const int dst,
                                        const int tag,
                                        sim_request* const request,
                                        void (*msg_handler)(void*),
                                        void* const fun_arg) {
    // query chunk id
    const auto src = sim_comm_get_rank();
    const auto chunk_id =
        ReconfigurableNetworkApi::chunk_id_generator.create_send_chunk_id(
            tag, src, dst, count);

    // search tracker
    const auto entry =
        callback_tracker.search_entry(tag, src, dst, count, chunk_id);
    if (entry.has_value()) {
        // recv operation already issued.
        // register send callback
        entry.value()->register_send_callback(msg_handler, fun_arg);
    } else {
        // recv operation not issued yet
        // create new entry and insert callback
        auto* const new_entry =
            callback_tracker.create_new_entry(tag, src, dst, count, chunk_id);
        new_entry->register_send_callback(msg_handler, fun_arg);
    }

    // create chunk
    auto chunk_arrival_arg = std::tuple(tag, src, dst, count, chunk_id);
    auto arg = std::make_unique<decltype(chunk_arrival_arg)>(chunk_arrival_arg);
    const auto arg_ptr = static_cast<void*>(arg.release());
    const auto route = tm->route(src, dst);
    auto chunk = std::make_unique<Chunk>(
        count, route, ReconfigurableNetworkApi::process_chunk_arrival,
        arg_ptr);

    // initiate transmission from src -> dst.
    tm->send(std::move(chunk));

    // return
    return 0;
}
