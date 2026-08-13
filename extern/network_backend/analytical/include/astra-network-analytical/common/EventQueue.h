/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/Type.h"
#include <cstdint>
#include <queue>
#include <unordered_map>
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
    /// One scheduled event. `seq` is a monotonic insertion counter: the pop
    /// order is (time asc, seq asc), so same-time events pop in insertion
    /// order -- the same FIFO the previous map<time, EventList> produced.
    struct HeapEvent {
        EventTime time;
        std::uint64_t seq;
        Callback callback;
        CallbackArg callback_arg;
    };

    /// strict (time, seq) ordering -- seq is unique, so this is a total
    /// order and the pop sequence is structure-independent
    static bool event_before(const HeapEvent& a, const HeapEvent& b) noexcept {
        return a.time < b.time || (a.time == b.time && a.seq < b.seq);
    }

    /// Two-level calendar structure. A single global heap paid a deep,
    /// cache-cold sift per pop once congestion booked 10^4-10^5 events into
    /// the future (26% of scatter-cell cycles at 16^3). Instead, events are
    /// bucketed by coarse time epoch (time >> kEpochShift): only the epochs
    /// at/below `near_epoch` live in the sorted `near` heap; later epochs
    /// sit in unsorted per-epoch `far` buckets (O(1) append). When `near`
    /// drains, the smallest far bucket is heapified wholesale. Every event
    /// still pops in strict (time, seq) order -- all times in a far bucket
    /// exceed every time in `near`, and a heap pops a total order
    /// structure-independently -- so results are byte-identical to the
    /// single-heap (and original map) implementation.
    ///
    /// Epoch width 2^12 ns (~4 us): narrower than a typical hop's
    /// latency+serialization delta, so even next-hop arrival events land in
    /// a *future* bucket (O(1) append) instead of the near heap, and the
    /// near heap stays a few hundred events (L1/L2-resident, 2-4 levels).
    /// Measured on the 16^3 uniform1024 random probe: 2^18 left 73% of
    /// ~4.5k pending events in near (sift_down still 25% of cycles); 2^12
    /// empties near to the current bucket's worth. Degenerate widths stay
    /// correct: too-wide collapses to the old single heap, too-narrow costs
    /// one small heapify per few events.
    static constexpr unsigned kEpochShift = 12;

    static std::uint64_t epoch_of(const EventTime time) noexcept {
        return static_cast<std::uint64_t>(time) >> kEpochShift;
    }

    /// 4-ary min-heap over `side`: half the depth of a binary heap and the
    /// four children of a node are adjacent in memory, which roughly halves
    /// the cache-missy element moves per pop.
    static constexpr std::size_t kHeapArity = 4;
    void sift_up(std::size_t i) noexcept;
    void sift_down(std::size_t i) noexcept;

    /// move the smallest far bucket into the (empty) near stage
    void refill_near() noexcept;

    /// near-stage emptiness (drain cursor exhausted and side heap empty)
    [[nodiscard]] bool near_empty() const noexcept {
        return cursor == drain.size() && side.empty();
    }

    /// smallest pending near event (min of the two stage heads)
    [[nodiscard]] const HeapEvent& front() const noexcept {
        if (cursor < drain.size() && (side.empty() || event_before(drain[cursor], side.front()))) {
            return drain[cursor];
        }
        return side.front();
    }

    /// remove front() from its stage
    void pop_front() noexcept;

    /// current time of the event queue
    EventTime current_time;

    /// next insertion sequence number
    std::uint64_t next_seq;

    /// Near stage, part 1: the adopted bucket, sorted ascending by
    /// (time, seq) once at refill and consumed front-to-back -- a pop is a
    /// cursor increment instead of a heap sift (one sort pass per bucket
    /// replaces a cache-missy sift per event).
    std::vector<HeapEvent> drain;
    std::size_t cursor;

    /// Near stage, part 2: events scheduled INTO the near window after the
    /// bucket was sorted (same-tick watermark deferrals and next-hop
    /// arrivals landing a few hundred ns out). Small 4-ary min-heap;
    /// front() merges the two stages, preserving the exact total order.
    std::vector<HeapEvent> side;

    /// highest epoch admitted to `near`; far buckets are strictly above it
    std::uint64_t near_epoch;

    /// far events, unsorted, keyed by epoch; buckets are non-empty
    std::unordered_map<std::uint64_t, std::vector<HeapEvent>> far;

    /// total events across all far buckets
    std::size_t far_count;

    /// epochs present in `far`, smallest first. An epoch is pushed exactly
    /// once (bucket creation) and popped exactly once (refill), and new
    /// epochs are always > near_epoch, so entries are unique.
    std::priority_queue<std::uint64_t, std::vector<std::uint64_t>, std::greater<>> far_epochs;
};

}  // namespace NetworkAnalytical
