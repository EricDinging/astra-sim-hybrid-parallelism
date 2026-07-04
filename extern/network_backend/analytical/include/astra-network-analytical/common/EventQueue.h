/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/EventList.h"
#include "common/Type.h"
#include <map>

namespace NetworkAnalytical {

/**
 * EventQueue manages scheduled EventLists.
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
    /// current time of the event queue
    EventTime current_time;

    /// pending EventLists keyed (and ordered) by event time. A map makes
    /// schedule_event O(log K) instead of a linear scan over all pending
    /// timestamps; iteration order (ascending time) and same-time FIFO
    /// semantics are identical to the previous sorted list.
    std::map<EventTime, EventList> event_queue;
};

}  // namespace NetworkAnalytical
