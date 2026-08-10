/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/EventQueue.h"
#include <cassert>

using namespace NetworkAnalytical;

EventQueue::EventQueue() noexcept : current_time(0), next_seq(0), near_epoch(0), far_count(0) {}

EventTime EventQueue::get_current_time() const noexcept {
    return current_time;
}

bool EventQueue::finished() const noexcept {
    // check whether event queue is empty
    return near.empty() && far_count == 0;
}

void EventQueue::sift_up(std::size_t i) noexcept {
    const HeapEvent v = near[i];
    while (i > 0) {
        const std::size_t parent = (i - 1) / kHeapArity;
        if (!event_before(v, near[parent])) {
            break;
        }
        near[i] = near[parent];
        i = parent;
    }
    near[i] = v;
}

void EventQueue::sift_down(std::size_t i) noexcept {
    const std::size_t n = near.size();
    const HeapEvent v = near[i];
    for (;;) {
        const std::size_t first_child = kHeapArity * i + 1;
        if (first_child >= n) {
            break;
        }
        std::size_t min_child = first_child;
        const std::size_t end = (first_child + kHeapArity < n) ? first_child + kHeapArity : n;
        for (std::size_t c = first_child + 1; c < end; ++c) {
            if (event_before(near[c], near[min_child])) {
                min_child = c;
            }
        }
        if (!event_before(near[min_child], v)) {
            break;
        }
        near[i] = near[min_child];
        i = min_child;
    }
    near[i] = v;
}

void EventQueue::refill_near() noexcept {
    assert(near.empty());
    assert(far_count > 0);

    // adopt the smallest far bucket wholesale
    const std::uint64_t e = far_epochs.top();
    far_epochs.pop();
    const auto it = far.find(e);
    assert(it != far.end() && !it->second.empty());
    near = std::move(it->second);
    far.erase(it);
    far_count -= near.size();
    near_epoch = e;

    // heapify: sift down every internal node, bottom-up. The heap pops a
    // strict total order, so the pop sequence is independent of this build
    // order.
    const std::size_t n = near.size();
    if (n > 1) {
        for (std::size_t i = (n - 2) / kHeapArity + 1; i-- > 0;) {
            sift_down(i);
        }
    }
}

void EventQueue::proceed() noexcept {
    // to proceed, next event should exist
    assert(!finished());

    if (near.empty()) {
        refill_near();
    }

    // Advance the clock to the earliest pending time, then invoke every
    // event at that time that was already scheduled when this call began.
    // The seq watermark reproduces the previous detach-by-move semantics
    // exactly: events scheduled *during* invocation at the same timestamp
    // get seq >= watermark and are left for a subsequent proceed(), just as
    // they used to land in a fresh map entry after the detach.
    // (Same-time events always share the front epoch, so they are all in
    // `near`: every far event's time strictly exceeds every near time.)
    const EventTime time = near.front().time;
    assert(time >= current_time);
    current_time = time;
    const std::uint64_t watermark = next_seq;

    while (!near.empty() && near.front().time == time && near.front().seq < watermark) {
        // Pop the root: move the last element into its place and sift down.
        // seq is unique, so (time, seq) is a strict total order and the pop
        // sequence is identical for any correct priority-queue structure.
        const HeapEvent event = near.front();
        near.front() = near.back();
        near.pop_back();
        if (!near.empty()) {
            sift_down(0);
            // The next event to fire is now known; its callback argument
            // (typically a Chunk or handler struct, cold after thousands of
            // intervening events) is prefetched behind the current callback's
            // execution. Semantics-free.
            __builtin_prefetch(near.front().callback_arg);
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
        near.push_back({event_time, next_seq++, callback, callback_arg});
        sift_up(near.size() - 1);
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
