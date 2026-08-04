/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/CommonNetworkApi.hh"
#include <cassert>

using namespace AstraSim;
using namespace AstraSimAnalytical;
using namespace NetworkAnalytical;

std::shared_ptr<EventQueue> CommonNetworkApi::event_queue = nullptr;

ChunkIdGenerator CommonNetworkApi::chunk_id_generator = {};

CallbackTracker CommonNetworkApi::callback_tracker = {};

int CommonNetworkApi::dims_count = -1;

std::vector<Bandwidth> CommonNetworkApi::bandwidth_per_dim = {};

void CommonNetworkApi::set_event_queue(
    std::shared_ptr<EventQueue> event_queue_ptr) noexcept {
    assert(event_queue_ptr != nullptr);

    CommonNetworkApi::event_queue = std::move(event_queue_ptr);
}

CallbackTracker& CommonNetworkApi::get_callback_tracker() noexcept {
    return callback_tracker;
}

void CommonNetworkApi::process_chunk_arrival(void* args) noexcept {
    assert(args != nullptr);

    // parse chunk data: the tracker entry pointer rides in the arg (set by
    // sim_send), so no hash lookup is needed here
    auto* const data = static_cast<ChunkArrivalArg*>(args);
    auto* const entry = data->entry;
    const auto tag = data->tag;
    const auto src = data->src;
    const auto dest = data->dst;
    const auto count = data->count;
    const auto chunk_id = data->chunk_id;
    delete data;

    assert(entry != nullptr);  // entry must exist

    // if both callbacks are registered, invoke both callbacks
    if (entry->both_callbacks_registered()) {
        entry->invoke_send_handler();
        entry->invoke_recv_handler();

        // remove entry; also let the chunk-id generator drop the key once
        // all of its chunks retired, so the map stops growing for the whole
        // run. Read the generator-entry pointer before the pop frees the
        // tracker entry.
        auto* const gen_entry = entry->get_generator_entry();
        auto& tracker = CommonNetworkApi::get_callback_tracker();
        tracker.pop_entry(tag, src, dest, count, chunk_id);
        chunk_id_generator.retire_chunk(gen_entry, tag, src, dest, count);
    } else {
        // run only send callback, as recv is not ready yet.
        entry->invoke_send_handler();

        // mark the transmission as finished
        // so that recv callback will be invoked immediately
        // when sim_recv() is called
        entry->set_transmission_finished();
    }
}

CommonNetworkApi::CommonNetworkApi(const int rank) noexcept
    : AstraNetworkAPI(rank) {
    assert(rank >= 0);
}

timespec_t CommonNetworkApi::sim_get_time() {
    // get current time from event queue
    const auto current_time = event_queue->get_current_time();

    // return the current time in ASTRA-sim format
    const auto astra_sim_time = static_cast<double>(current_time);
    return {NS, astra_sim_time};
}

Tick CommonNetworkApi::sim_get_time_ns() {
    // CLOCK_PERIOD is 1, so the historical path (uint64 -> long double,
    // divide by 1, truncate back) yields this same integer; return it
    // directly and skip the x87 round-trip.
    static_assert(AstraSim::CLOCK_PERIOD == 1,
                  "shortcut below assumes 1ns ticks");
    return event_queue->get_current_time();
}

void CommonNetworkApi::sim_schedule(const timespec_t delta,
                                    void (*fun_ptr)(void*),
                                    void* const fun_arg) {
    assert(delta.time_res == NS);
    assert(fun_ptr != nullptr);

    // calculate absolute event time
    const auto current_time = sim_get_time();
    const auto event_time = current_time.time_val + delta.time_val;
    const auto event_time_ns = static_cast<EventTime>(event_time);

    // schedule the event to the event queue
    assert(event_time_ns >= event_queue->get_current_time());
    event_queue->schedule_event(event_time_ns, fun_ptr, fun_arg);
}

int CommonNetworkApi::sim_recv(void* const buffer,
                               const uint64_t count,
                               const int type,
                               const int src,
                               const int tag,
                               sim_request* const request,
                               void (*msg_handler)(void*),
                               void* const fun_arg) {
    // query chunk id (and the generator entry, for probe-free retire)
    const auto dst = sim_comm_get_rank();
    const auto [chunk_id, gen_entry] =
        CommonNetworkApi::chunk_id_generator.create_recv_chunk_id(tag, src, dst,
                                                                  count);

    // single probe: get the entry, creating it if send() wasn't called yet
    const auto [it, existed] =
        callback_tracker.find_or_create_entry(tag, src, dst, count, chunk_id);
    auto* const entry = &(it->second);
    if (existed && entry->is_transmission_finished()) {
        // send() already invoked and transmission already finished:
        // run callback immediately

        // pop entry by iterator (no re-hash) and retire the chunk-id key
        // via the generator-entry pointer when fully drained
        callback_tracker.erase_entry(it);
        chunk_id_generator.retire_chunk(gen_entry, tag, src, dst, count);

        // run recv callback immediately
        const auto delta = timespec_t{NS, 0};
        sim_schedule(delta, msg_handler, fun_arg);
    } else {
        // transmission not finished (or send() not yet called on the
        // just-created entry): just register the callback
        entry->set_generator_entry(gen_entry);
        entry->register_recv_callback(msg_handler, fun_arg);
    }

    // return
    return 0;
}

double CommonNetworkApi::get_BW_at_dimension(const int dim) {
    assert(0 <= dim && dim < dims_count);

    // return bandwidth of the requested dimension
    return bandwidth_per_dim[dim];
}
