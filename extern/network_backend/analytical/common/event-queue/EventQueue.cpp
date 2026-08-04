/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/EventQueue.h"
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

void EventQueue::sift_up(std::size_t i) noexcept {
    const HeapEvent v = heap[i];
    while (i > 0) {
        const std::size_t parent = (i - 1) / kHeapArity;
        if (!event_before(v, heap[parent])) {
            break;
        }
        heap[i] = heap[parent];
        i = parent;
    }
    heap[i] = v;
}

void EventQueue::sift_down(std::size_t i) noexcept {
    const std::size_t n = heap.size();
    const HeapEvent v = heap[i];
    for (;;) {
        const std::size_t first_child = kHeapArity * i + 1;
        if (first_child >= n) {
            break;
        }
        std::size_t min_child = first_child;
        const std::size_t end = (first_child + kHeapArity < n) ? first_child + kHeapArity : n;
        for (std::size_t c = first_child + 1; c < end; ++c) {
            if (event_before(heap[c], heap[min_child])) {
                min_child = c;
            }
        }
        if (!event_before(heap[min_child], v)) {
            break;
        }
        heap[i] = heap[min_child];
        i = min_child;
    }
    heap[i] = v;
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
        // Pop the root: move the last element into its place and sift down.
        // seq is unique, so (time, seq) is a strict total order and the pop
        // sequence is identical for any correct priority-queue structure.
        const HeapEvent event = heap.front();
        heap.front() = heap.back();
        heap.pop_back();
        if (!heap.empty()) {
            sift_down(0);
            // The next event to fire is now known; its callback argument
            // (typically a Chunk or handler struct, cold after thousands of
            // intervening events) is prefetched behind the current callback's
            // execution. Semantics-free.
            __builtin_prefetch(heap.front().callback_arg);
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

    heap.push_back({event_time, next_seq++, callback, callback_arg});
    sift_up(heap.size() - 1);
}
