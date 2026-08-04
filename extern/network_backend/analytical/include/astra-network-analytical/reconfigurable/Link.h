/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/EventQueue.h"
#include "common/Type.h"
#include "reconfigurable/Type.h"
#include <functional>
#include <memory>

using namespace NetworkAnalytical;

namespace NetworkAnalyticalReconfigurable {

/// Argument of Device::link_become_free events. One instance is preallocated
/// per link (owned by the Link, set at Device::connect) instead of being
/// heap-allocated per scheduled event: the owning Device outlives every event
/// (devices live in Topology::devices for the whole run), and the arg is
/// read-only in the callback, so all events of a link can share it.
struct LinkFreeCallbackArg {
    Device* device;
    DeviceId link_id;
};

/**
 * Link models physical links between two devices.
 */
class Link {
  public:
    static int num_drained_links;

    static std::function<void()> increment_callback;

    /**
     * Set the event queue to be used by the link.
     *
     * @param event_queue_ptr pointer to the event queue
     */
    static void set_event_queue(std::shared_ptr<EventQueue> event_queue_ptr) noexcept;

    /**
     * Constructor.
     *
     * @param bandwidth bandwidth of the link
     * @param latency latency of the link
     */
    Link(Bandwidth bandwidth, Latency latency) noexcept;

    /**
     * Check if the link is occupied right now: either flagged busy (mid-
     * reconfiguration downtime, or serving a cold-path queued transmission)
     * or arithmetically booked by hot-path sends (now < next_free_time).
     * This is the occupancy answer wiring/drain checks and the fluid rate
     * computation want.
     *
     * @return true if the link is occupied, false otherwise
     */
    [[nodiscard]] bool is_busy() const noexcept;

    /**
     * The busy FLAG only (reconfiguration downtime / cold-path transmission
     * in flight), excluding arithmetic occupancy. Device::send uses this to
     * decide hot vs cold path: hot-path sends append to the arithmetic
     * booking themselves, so arithmetic occupancy must not divert them.
     */
    [[nodiscard]] bool is_flag_busy() const noexcept {
        return busy;
    }

    /**
     * Busy-until arithmetic: the time this link finishes everything booked
     * on it so far (0 = never booked). Hot-path sends start at
     * max(now, next_free_time) and push it forward; cold-path transmissions
     * and reconfigures keep it in sync so hot sends after a cold stream
     * cannot start early.
     */
    [[nodiscard]] EventTime next_free_time() const noexcept {
        return next_free_time_;
    }
    void book_until(EventTime t) noexcept {
        if (t > next_free_time_) {
            next_free_time_ = t;
        }
    }

    /**
     * Try to send a chunk through the link.
     * - If the link is free, service the chunk immediately.
     * - If the link is busy, add the chunk to the pending chunks list.
     *
     * @param chunk the chunk to be served by the link
     */
    unsigned long send(std::unique_ptr<Chunk> chunk) noexcept;

    /**
     * Busy-until hot path: book the chunk's transmission arithmetically.
     * Starts at max(now, next_free_time), schedules ONLY the chunk-arrival
     * event, and advances next_free_time by serialization + 2x latency --
     * the identical start/arrival/occupancy times the event-driven scheme
     * produced, with one event per hop instead of two. Returns the new
     * next_free_time.
     */
    EventTime book_transmission(std::unique_ptr<Chunk> chunk) noexcept;

    /**
     * Set the link as busy.
     */
    void set_busy() noexcept;

    /**
     * Set the link as free.
     */
    void set_free() noexcept;

    /**
     * Reconfigure the link.
     */
    unsigned long reconfigure(Bandwidth bandwidth, Latency latency, Latency reconfig_time) noexcept;

    /**
     * Get the bandwidth of the link in GB/s.
     *
     * @return bandwidth of the link
     */
    [[nodiscard]] Bandwidth get_bandwidth() const noexcept {
        return bandwidth;
    }

    /**
     * Get the latency of the link in ns.
     *
     * @return latency of the link
     */
    [[nodiscard]] Latency get_latency() const noexcept {
        return latency;
    }

    static void schedule_event(EventTime event_time, Callback callback, void* const arg) noexcept;

    /// Current simulation time from the shared event queue (for occupancy
    /// checks outside the Link itself).
    [[nodiscard]] static EventTime current_time() noexcept;

    /// Bind the preallocated link-free callback arg to the owning device
    /// (called once, from Device::connect).
    void set_free_callback_arg(Device* device, DeviceId link_id) noexcept {
        free_callback_arg_ = {device, link_id};
    }

    /// The preallocated argument to pass to Device::link_become_free events.
    [[nodiscard]] void* free_callback_arg() noexcept {
        return &free_callback_arg_;
    }

    /// Fluid-model bookkeeping: number of flows currently occupying this
    /// link, maintained by FlowEngine at flow register/deregister. Stored
    /// here so the per-flow fair-share computation (min over links of
    /// bw / count, re-run for every affected flow on every flow start and
    /// finish) reads a member instead of probing FlowEngine's per-link
    /// hash map. Unused (always 0) under the serial model.
    [[nodiscard]] int fluid_flow_count() const noexcept {
        return fluid_flow_count_;
    }
    void fluid_flow_inc() noexcept {
        ++fluid_flow_count_;
    }
    void fluid_flow_dec() noexcept {
        --fluid_flow_count_;
    }

  private:
    // Send-hot fields first (one cache line): Device::send reads busy,
    // bandwidth, next_free_time_, bandwidth_Bpns and latency on every hop.
    // Pure layout change, no semantics.

    /// flag to indicate if the link is busy
    bool busy;

    /// bandwidth of the link in GB/s
    Bandwidth bandwidth;

    /// Busy-until arithmetic occupancy (see next_free_time()).
    EventTime next_free_time_ = 0;

    /// bandwidth of the link in B/ns, used in actual computation
    Bandwidth bandwidth_Bpns;

    /// latency of the link in ns
    Latency latency;

    /// preallocated argument shared by all link-free events of this link
    LinkFreeCallbackArg free_callback_arg_{nullptr, -1};

    /// fluid-model active-flow count (see fluid_flow_count())
    int fluid_flow_count_ = 0;

    /// event queue Link uses to schedule events
    static std::shared_ptr<EventQueue> event_queue;

    /**
     * Compute the serialization delay of a chunk on the link.
     * i.e., serialization delay = (chunk size) / (link bandwidth)
     *
     * @param chunk_size size of the target chunk
     * @return serialization delay of the chunk
     */
    [[nodiscard]] EventTime serialization_delay(ChunkSize chunk_size) const noexcept;

    /**
     * Compute the communication delay of a chunk.
     * i.e., communication delay = (link latency) + (serialization delay)
     *
     * @param chunk_size size of the target chunk
     * @return communication delay of the chunk
     */
    [[nodiscard]] EventTime communication_delay(ChunkSize chunk_size) const noexcept;

    /**
     * Schedule the transmission of a chunk.
     * - Set the link as busy.
     * - Link becomes free after the serialization delay.
     * - Chunk arrives next node after the communication delay.
     *
     * @param chunk chunk to be transmitted
     */
    unsigned long schedule_chunk_transmission(std::unique_ptr<Chunk> chunk) noexcept;
};

}  // namespace NetworkAnalyticalReconfigurable
