/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/EventQueue.h"
#include <algorithm>
#include <cassert>

using namespace NetworkAnalytical;

EventQueue::EventQueue() noexcept : current_time(0), next_seq(0) {}

EventTime EventQueue::get_current_time() const noexcept {
    return current_time;
}

bool EventQueue::finished() const noexcept {
    // check whether event queue is empty
    return heap.empty();
}

void EventQueue::proceed() noexcept {
    // to proceed, next event should exist
    assert(!finished());

    // Advance the clock to the earliest pending time, then invoke every
    // event at that time that was already scheduled when this call began.
    // The seq watermark reproduces the previous detach-by-move semantics
    // exactly: events scheduled *during* invocation at the same timestamp
    // get seq >= watermark and are left for a subsequent proceed(), just as
    // they used to land in a fresh map entry after the detach.
    const EventTime time = heap.front().time;
    assert(time >= current_time);
    current_time = time;
    const std::uint64_t watermark = next_seq;

    while (!heap.empty() && heap.front().time == time && heap.front().seq < watermark) {
        std::pop_heap(heap.begin(), heap.end(), heap_after);
        const HeapEvent event = heap.back();
        heap.pop_back();
        (*event.callback)(event.callback_arg);
    }
}

void EventQueue::schedule_event(const EventTime event_time,
                                const Callback callback,
                                const CallbackArg callback_arg) noexcept {
    // time should be at least larger than current time
    assert(event_time >= current_time);
    assert(callback != nullptr);

    heap.push_back({event_time, next_seq++, callback, callback_arg});
    std::push_heap(heap.begin(), heap.end(), heap_after);
}
