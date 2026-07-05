/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/CallbackTrackerEntry.hh"
#include "common/ChunkIdGenerator.hh"
#include <functional>
#include <unordered_map>

namespace AstraSimAnalytical {

/**
 * CallbackTracker keeps track of sim_send() and sim_recv() callbacks of each
 * chunk identified by (tag, src, dest, chunk_size, chunk_id) tuple.
 */
class CallbackTracker {
  public:
    /// Key = (tag, src, dest, chunk_size, chunk_id)
    using Key = std::tuple<int, int, int, ChunkSize, int>;

    /// Hash functor for the tuple key. std::unordered_map has no built-in hash
    /// for std::tuple, so combine the five field hashes (boost-style mix).
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            std::size_t h = 0;
            const auto mix = [&h](const std::size_t v) {
                h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            };
            mix(std::hash<int>{}(std::get<0>(key)));
            mix(std::hash<int>{}(std::get<1>(key)));
            mix(std::hash<int>{}(std::get<2>(key)));
            mix(std::hash<ChunkSize>{}(std::get<3>(key)));
            mix(std::hash<int>{}(std::get<4>(key)));
            return h;
        }
    };

    CallbackTracker() noexcept;

    /**
     * Search for the entry identified by (tag, src, dest, chunk_size, chunk_id)
     * tuple.
     *
     * @param tag tag of the sim_send() or sim_recv() call
     * @param src src NPU ID of the sim_send() or sim_recv() call
     * @param dest dest NPU ID of the sim_send() or sim_recv() call
     * @param chunk_size chunk size of the sim_send() or sim_recv() call
     * @param chunk_id id of the chunk
     * @return the found entry if exists, std::nullopt otherwise
     */
    std::optional<CallbackTrackerEntry*> search_entry(int tag,
                                                      int src,
                                                      int dest,
                                                      ChunkSize chunk_size,
                                                      int chunk_id) noexcept;

    /**
     * Create a new entry identified by (tag, src, dest, chunk_size, chunk_id)
     * tuple.
     *
     * @param tag tag of the sim_send() or sim_recv() call
     * @param src src NPU ID of the sim_send() or sim_recv() call
     * @param dest dest NPU ID of the sim_send() or sim_recv() call
     * @param chunk_size chunk size of the sim_send() or sim_recv() call
     * @param chunk_id id of the chunk
     * @return the created entry
     */
    CallbackTrackerEntry* create_new_entry(int tag,
                                           int src,
                                           int dest,
                                           ChunkSize chunk_size,
                                           int chunk_id) noexcept;

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
    [[nodiscard]] const std::unordered_map<Key, CallbackTrackerEntry, KeyHash>&
    entries() const noexcept {
        return tracker;
    }

  private:
    /// map from (tag, src, dest, chunk_size, chunk_id) tuple to
    /// CallbackTrackerEntry
    std::unordered_map<Key, CallbackTrackerEntry, KeyHash> tracker;
};

}  // namespace AstraSimAnalytical
