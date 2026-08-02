/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/EventList.h"
#include <cassert>
#include <iostream>

using namespace NetworkAnalytical;

EventList::EventList(const EventTime event_time) noexcept : event_time(event_time) {
    assert(event_time >= 0);
}

EventTime EventList::get_event_time() const noexcept {
    return event_time;
}

void EventList::add_event(const Callback callback, const CallbackArg callback_arg) noexcept {
    assert(callback != nullptr);

    // add the event to the event list
    events.emplace_back(callback, callback_arg);
}

void EventList::invoke_events() noexcept {
    // Invoke all events in insertion order. This list is already detached
    // from the queue map when invoked (EventQueue::proceed moves it out), so
    // same-time events scheduled DURING invocation go to a fresh map entry
    // processed by a later proceed() — the vector cannot grow while iterating.
    for (auto& event : events) {
        event.invoke_event();
    }
    events.clear();
}
