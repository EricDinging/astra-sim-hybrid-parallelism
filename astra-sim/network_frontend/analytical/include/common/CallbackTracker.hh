/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/CallbackTrackerEntry.hh"
#include "common/ChunkIdGenerator.hh"
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>

namespace AstraSimAnalytical {

/**
 * CallbackTracker keeps track of sim_send() and sim_recv() callbacks of each
 * chunk identified by (tag, src, dest, chunk_size, chunk_id) tuple.
 */
class CallbackTracker {
  public:
    /// Key = (tag, src, dest, chunk_size, chunk_id)
    using Key = std::tuple<int, int, int, ChunkSize, int>;

    /// Hash functor for the tuple key. std::unordered_map has no built-in
    /// hash for std::tuple. This is a per-chunk hot path (one probe per
    /// sim_send/sim_recv/completion), so pack the five fields into two
    /// 64-bit words and run a splitmix64 finalizer on each -- two multiplies
    /// instead of five std::hash calls + five mix rounds. Hash choice only
    /// affects bucket placement, never map contents: results are identical.
    struct KeyHash {
        static std::size_t mix64(std::uint64_t x) noexcept {
            x += 0x9e3779b97f4a7c15ULL;
            x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
            x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
            return static_cast<std::size_t>(x ^ (x >> 31));
        }
        std::size_t operator()(const Key& key) const noexcept {
            const auto a = (static_cast<std::uint64_t>(
                                static_cast<std::uint32_t>(std::get<0>(key)))
                            << 32) |
                           static_cast<std::uint32_t>(std::get<1>(key));
            const auto b = (static_cast<std::uint64_t>(
                                static_cast<std::uint32_t>(std::get<2>(key)))
                            << 32) |
                           static_cast<std::uint32_t>(std::get<4>(key));
            return mix64(a) ^
                   mix64(b ^ static_cast<std::uint64_t>(std::get<3>(key)));
        }
    };

    /// underlying map type (also read by the deadlock post-mortem dump)
    using Map = std::unordered_map<Key, CallbackTrackerEntry, KeyHash>;
    using Iterator = Map::iterator;

    CallbackTracker() noexcept;

    /**
     * Get the entry identified by (tag, src, dest, chunk_size, chunk_id),
     * creating it if absent — a single hash probe. Returns the iterator so
     * callers that erase the same entry in the same call (sim_recv's
     * recv-after-arrival path) can do so without a second probe via
     * erase_entry().
     *
     * @return pair of (iterator to the entry, whether it already existed)
     */
    std::pair<Iterator, bool> find_or_create_entry(int tag,
                                                   int src,
                                                   int dest,
                                                   ChunkSize chunk_size,
                                                   int chunk_id) noexcept;

    /**
     * Remove the entry the iterator points at, without hashing the key again.
     * Only valid for an iterator obtained in the same call with no
     * intervening insertion (rehash invalidates iterators).
     *
     * @param entry iterator to the entry to remove
     */
    void erase_entry(Iterator entry) noexcept;

    /**
     * Remove the entry identified by (tag, src, dest, chunk_size, chunk_id)
     * tuple.
     *
     * @param tag tag of the sim_send() or sim_recv() call
     * @param src src NPU ID of the sim_send() or sim_recv() call
     * @param dest dest NPU ID of the sim_send() or sim_recv() call
     * @param chunk_size chunk size of the sim_send() or sim_recv() call
     * @param chunk_id id of the chunk
     */
    void pop_entry(int tag,
                   int src,
                   int dest,
                   ChunkSize chunk_size,
                   int chunk_id) noexcept;

    // Read-only view of unresolved entries, for deadlock post-mortem dumps.
    [[nodiscard]] const Map& entries() const noexcept {
        return tracker;
    }

  private:
    /// map from (tag, src, dest, chunk_size, chunk_id) tuple to
    /// CallbackTrackerEntry
    Map tracker;
};

}  // namespace AstraSimAnalytical
