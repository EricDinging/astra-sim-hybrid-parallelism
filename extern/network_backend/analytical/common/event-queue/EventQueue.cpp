/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/EventQueue.h"
#include <cassert>
#include <stdio.h>

using namespace NetworkAnalytical;

EventQueue::EventQueue() noexcept : current_time(0) {}

EventTime EventQueue::get_current_time() const noexcept {
    return current_time;
}

bool EventQueue::finished() const noexcept {
    // check whether event queue is empty
    return event_queue.empty();
}

void EventQueue::proceed() noexcept {
    // to proceed, next event should exist
    assert(!finished());

    // detach the earliest event list (move, not copy: it holds every event
    // registered at that timestamp), then advance the clock and invoke.
    // Events scheduled during invocation at the same timestamp create a new
    // map entry processed by a subsequent proceed(), exactly like the
    // previous list-based implementation.
    auto first_it = event_queue.begin();
    EventList current_event_list = std::move(first_it->second);
    event_queue.erase(first_it);

    assert(current_event_list.get_event_time() >= current_time);
    current_time = current_event_list.get_event_time();

    current_event_list.invoke_events();
}

void EventQueue::schedule_event(const EventTime event_time,
                                const Callback callback,
                                const CallbackArg callback_arg) noexcept {
    // time should be at least larger than current time
    assert(event_time >= current_time);

    // O(log K): find-or-create the event list for this timestamp.
    const auto event_list_it = event_queue.try_emplace(event_time, event_time).first;
    event_list_it->second.add_event(callback, callback_arg);
}
