/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/EventQueue.h"
#include <algorithm>
#include <cassert>

using namespace NetworkAnalytical;

EventQueue::EventQueue() noexcept : current_time(0), next_seq(0), cursor(0), near_epoch(0), far_count(0) {}

EventTime EventQueue::get_current_time() const noexcept {
    return current_time;
}

bool EventQueue::finished() const noexcept {
    // check whether event queue is empty
    return near_empty() && far_count == 0;
}

void EventQueue::sift_up(std::size_t i) noexcept {
    const HeapEvent v = side[i];
    while (i > 0) {
        const std::size_t parent = (i - 1) / kHeapArity;
        if (!event_before(v, side[parent])) {
            break;
        }
        side[i] = side[parent];
        i = parent;
    }
    side[i] = v;
}

void EventQueue::sift_down(std::size_t i) noexcept {
    const std::size_t n = side.size();
    const HeapEvent v = side[i];
    for (;;) {
        const std::size_t first_child = kHeapArity * i + 1;
        if (first_child >= n) {
            break;
        }
        std::size_t min_child = first_child;
        const std::size_t end = (first_child + kHeapArity < n) ? first_child + kHeapArity : n;
        for (std::size_t c = first_child + 1; c < end; ++c) {
            if (event_before(side[c], side[min_child])) {
                min_child = c;
            }
        }
        if (!event_before(side[min_child], v)) {
            break;
        }
        side[i] = side[min_child];
        i = min_child;
    }
    side[i] = v;
}

void EventQueue::pop_front() noexcept {
    if (cursor < drain.size() && (side.empty() || event_before(drain[cursor], side.front()))) {
        ++cursor;
        return;
    }
    side.front() = side.back();
    side.pop_back();
    if (!side.empty()) {
        sift_down(0);
    }
}

void EventQueue::refill_near() noexcept {
    assert(near_empty());
    assert(far_count > 0);

    // adopt the smallest far bucket wholesale
    const std::uint64_t e = far_epochs.top();
    far_epochs.pop();
    const auto it = far.find(e);
    assert(it != far.end() && !it->second.empty());
    drain = std::move(it->second);
    cursor = 0;
    far.erase(it);
    far_count -= drain.size();
    near_epoch = e;

    // One sort pass sets up cursor consumption for the whole bucket.
    // (time, seq) is a strict total order (seq unique), so the sorted
    // sequence -- and thus the pop order -- is deterministic.
    std::sort(drain.begin(), drain.end(), event_before);
}

void EventQueue::proceed() noexcept {
    // to proceed, next event should exist
    assert(!finished());

    if (near_empty()) {
        refill_near();
    }

    // Advance the clock to the earliest pending time, then invoke every
    // event at that time that was already scheduled when this call began.
    // The seq watermark reproduces the previous detach-by-move semantics
    // exactly: events scheduled *during* invocation at the same timestamp
    // get seq >= watermark and are left for a subsequent proceed(), just as
    // they used to land in a fresh map entry after the detach.
    // (Same-time events always share the front epoch, so they are all in
    // the near stage: every far event's time strictly exceeds every near
    // time.)
    const EventTime time = front().time;
    assert(time >= current_time);
    current_time = time;
    const std::uint64_t watermark = next_seq;

    while (!near_empty() && front().time == time && front().seq < watermark) {
        const HeapEvent event = front();
        pop_front();
        if (!near_empty()) {
            // The next event to fire is known; its callback argument
            // (typically a Chunk or handler struct, cold after thousands of
            // intervening events) is prefetched behind the current callback's
            // execution. Semantics-free.
            __builtin_prefetch(front().callback_arg);
        }
        (*event.callback)(event.callback_arg);
    }
}

void EventQueue::schedule_event(const EventTime event_time,
                                const Callback callback,
                                const CallbackArg callback_arg) noexcept {
    // time should be at least larger than current time
    assert(event_time >= current_time);
    assert(callback != nullptr);

    const std::uint64_t e = epoch_of(event_time);
    if (e <= near_epoch) {
        side.push_back({event_time, next_seq++, callback, callback_arg});
        sift_up(side.size() - 1);
        return;
    }

    // far epoch: O(1) unsorted append; sorted lazily at refill
    auto& bucket = far[e];
    if (bucket.empty()) {
        far_epochs.push(e);
    }
    bucket.push_back({event_time, next_seq++, callback, callback_arg});
    ++far_count;
}
