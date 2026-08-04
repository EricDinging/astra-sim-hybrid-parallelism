/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "reconfigurable/Link.h"
#include "common/HelperFunction.h"
#include "common/NetworkFunction.h"
#include "reconfigurable/Chunk.h"
#include "reconfigurable/Device.h"
#include <algorithm>
#include <cassert>
#include <iostream>

using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

// declaring static event_queue
std::shared_ptr<EventQueue> Link::event_queue;

int NetworkAnalyticalReconfigurable::Link::num_drained_links = 0;
std::function<void()> Link::increment_callback = []() {};

void Link::set_event_queue(std::shared_ptr<EventQueue> event_queue_ptr) noexcept {
    assert(event_queue_ptr != nullptr);

    // set the event queue
    Link::event_queue = std::move(event_queue_ptr);
}

void Link::schedule_event(EventTime event_time, Callback callback, void* const arg) noexcept {
    assert(event_queue != nullptr);
    assert(callback != nullptr);

    // schedule the event in the event queue
    Link::event_queue->schedule_event(event_time, callback, arg);
}

EventTime Link::current_time() noexcept {
    assert(event_queue != nullptr);
    return Link::event_queue->get_current_time();
}

Link::Link(const Bandwidth bandwidth, const Latency latency) noexcept
    : busy(false),
      bandwidth(bandwidth),
      latency(latency) {
    assert(bandwidth >= 0);
    assert(latency >= 0);

    // convert bandwidth from GB/s to B/ns
    bandwidth_Bpns = bw_GBps_to_Bpns(bandwidth);
}

bool Link::is_busy() const noexcept {
    // Occupied if flagged (reconfig downtime / cold-path transmission) or
    // arithmetically booked by hot-path sends.
    return busy || Link::event_queue->get_current_time() < next_free_time_;
}

unsigned long Link::send(std::unique_ptr<Chunk> chunk) noexcept {
    assert(chunk != nullptr);
    assert(!busy);

    return schedule_chunk_transmission(std::move(chunk));
}

EventTime Link::book_transmission(std::unique_ptr<Chunk> chunk) noexcept {
    assert(chunk != nullptr);
    assert(!busy);

    // Busy-until hot path: the link's occupancy is arithmetic, not an event.
    // The chunk starts when the link's booked work ends (or now, if idle),
    // exactly the moment the old link-free event chain would have started
    // it, so every timestamp matches the event-driven scheme. Only ONE event
    // is scheduled per hop (the chunk's arrival) -- no link-free event, no
    // pending-queue traffic.
    const auto now = Link::event_queue->get_current_time();
    const auto start = next_free_time_ > now ? next_free_time_ : now;

    const auto chunk_size = chunk->get_size();
    assert(chunk_size > 0);
    // One division serves both delays. Truncation points replicate the
    // communication_delay/serialization_delay pair exactly: (latency + d)
    // truncated for the arrival, d truncated (then + 2x latency) for the
    // occupancy window.
    const auto d = static_cast<Bandwidth>(chunk_size) / bandwidth_Bpns;
    const auto arrival = start + static_cast<EventTime>(latency + d);
    auto* const chunk_ptr = static_cast<void*>(chunk.release());
    Link::event_queue->schedule_event(arrival, Chunk::chunk_arrived_next_device, chunk_ptr);

    // Same occupancy window the event scheme used: serialization + 2x latency.
    next_free_time_ = start + static_cast<EventTime>(d) + 2 * static_cast<EventTime>(latency);
    return next_free_time_;
}

void Link::set_busy() noexcept {
    // set busy to true
    busy = true;
}

void Link::set_free() noexcept {
    // set busy to false
    busy = false;
}

EventTime Link::serialization_delay(const ChunkSize chunk_size) const noexcept {
    assert(chunk_size > 0);

    // calculate serialization delay
    const auto delay = static_cast<Bandwidth>(chunk_size) / bandwidth_Bpns;

    // return serialization delay in EventTime type
    return static_cast<EventTime>(delay);
}

EventTime Link::communication_delay(const ChunkSize chunk_size) const noexcept {
    assert(chunk_size > 0);

    // calculate communication delay
    const auto delay = latency + (static_cast<Bandwidth>(chunk_size) / bandwidth_Bpns);

    // return communication delay in EventTime type
    return static_cast<EventTime>(delay);
}

unsigned long Link::schedule_chunk_transmission(std::unique_ptr<Chunk> chunk) noexcept {
    assert(chunk != nullptr);

    // link should be free
    assert(!busy);

    // set link busy
    set_busy();

    // get metadata
    const auto chunk_size = chunk->get_size();
    const auto current_time = Link::event_queue->get_current_time();

    // schedule chunk arrival event
    const auto communication_time = communication_delay(chunk_size);
    const auto chunk_arrival_time = current_time + communication_time;
    auto* const chunk_ptr = static_cast<void*>(chunk.release());
    Link::event_queue->schedule_event(chunk_arrival_time, Chunk::chunk_arrived_next_device, chunk_ptr);

    // schedule link free time
    const auto serialization_time = serialization_delay(chunk_size);
    const auto link_free_time = current_time + serialization_time + 2 * latency;
    // Keep the busy-until arithmetic in sync with cold-path (event-driven)
    // transmissions, so hot-path sends after this queue drains cannot start
    // before it ends.
    book_until(static_cast<EventTime>(link_free_time));
    return link_free_time;
}

unsigned long Link::reconfigure(Bandwidth bandwidth, Latency latency, Latency reconfig_time) noexcept {
    if (bandwidth == this->bandwidth && latency == this->latency) {
        if (kVerboseLogging) {
            debug_print("No reconfiguration needed");
        }
        return Link::event_queue->get_current_time() + 1;
    }

    assert(!busy);
    const auto current_time = Link::event_queue->get_current_time();
    set_busy();

    if (kVerboseLogging) {
        debug_print("Reconfiguring link from bandwidth " + std::to_string(this->bandwidth) + " GB/s to " +
                    std::to_string(bandwidth) + " GB/s and latency " + std::to_string(this->latency) + " ns to " +
                    std::to_string(latency) + " ns at time " + std::to_string(current_time) + " ns");
    }

    this->bandwidth = bandwidth;
    this->latency = latency;
    this->bandwidth_Bpns = bw_GBps_to_Bpns(bandwidth);

    // printf("Link bandwidth updated to %.2f B/ns and latency updated to %.2f ns\n",
    //        this->bandwidth_Bpns, this->latency);

    const auto reconfig_completion_time = current_time + reconfig_time;
    // Downtime occupies the link in the busy-until arithmetic too.
    book_until(static_cast<EventTime>(reconfig_completion_time));
    return reconfig_completion_time;
}
