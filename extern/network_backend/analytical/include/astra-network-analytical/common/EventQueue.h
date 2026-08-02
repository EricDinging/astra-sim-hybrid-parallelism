/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/Type.h"
#include <cstdint>
#include <vector>

namespace NetworkAnalytical {

/**
 * EventQueue manages scheduled events.
 */
class EventQueue {
  public:
    /**
     * Constructor.
     */
    EventQueue() noexcept;

    /**
     * Get current event time of the event queue.
     *
     * @return current event time
     */
    [[nodiscard]] EventTime get_current_time() const noexcept;

    /**
     * Check all registered events are invoked.
     * i.e., check if the event queue is empty.
     *
     * @return true if the event queue is empty, false otherwise
     */
    [[nodiscard]] bool finished() const noexcept;

    /**
     * Proceed the event queue.
     * i.e., first update the current event time to the next registered event
     * time, and then invoke all events registered at the current updated event
     * time.
     */
    void proceed() noexcept;

    /**
     * Schedule an event with a given event time.
     *
     * @param event_time time of event
     * @param callback callback function pointer
     * @param callback_arg argument of the callback function
     */
    void schedule_event(EventTime event_time, Callback callback, CallbackArg callback_arg) noexcept;

  private:
    /// One scheduled event. `seq` is a monotonic insertion counter: heap
    /// order is (time asc, seq asc), so same-time events pop in insertion
    /// order -- the same FIFO the previous map<time, EventList> produced.
    struct HeapEvent {
        EventTime time;
        std::uint64_t seq;
        Callback callback;
        CallbackArg callback_arg;
    };

    /// min-heap comparator (std::*_heap build a max-heap w.r.t. the
    /// comparator, so "greater" yields a min-heap)
    static bool heap_after(const HeapEvent& a, const HeapEvent& b) noexcept {
        return a.time > b.time || (a.time == b.time && a.seq > b.seq);
    }

    /// current time of the event queue
    EventTime current_time;

    /// next insertion sequence number
    std::uint64_t next_seq;

    /// pending events as a flat binary heap ordered by (time, seq).
    /// Replaces map<EventTime, EventList>: steady state costs no per-event
    /// node allocation and no tree rebalance, only amortized vector pushes.
    std::vector<HeapEvent> heap;
};

}  // namespace NetworkAnalytical
