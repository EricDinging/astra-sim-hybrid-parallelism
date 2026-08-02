/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/ChunkIdGeneratorEntry.hh"
#include <astra-network-analytical/common/Type.h>
#include <cstdint>
#include <tuple>
#include <unordered_map>

using namespace NetworkAnalytical;

namespace AstraSimAnalytical {

/**
 * ChunkIdGenerator generates unique chunk id for sim_send() and sim_recv()
 * calls given (tag, src, dest, chunk_size) tuple.
 */
class ChunkIdGenerator {
  public:
    /// Key = (tag, src, dest, chunk_size)
    using Key = std::tuple<int, int, int, ChunkSize>;

    /// Hash functor for the tuple key. std::unordered_map has no built-in
    /// hash for std::tuple. Per-chunk hot path: pack the four fields into
    /// two 64-bit words and splitmix64 each -- two multiplies instead of
    /// four std::hash calls + four mix rounds. Hash choice only affects
    /// bucket placement, never map contents: results are identical.
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
                            << 32) ^
                           static_cast<std::uint64_t>(std::get<3>(key));
            return mix64(a) ^ mix64(b);
        }
    };

    /**
     * Constructor.
     */
    ChunkIdGenerator() noexcept;

    /**
     * Create unique chunk id for sim_send() call.
     *
     * @param tag tag of the sim_send() call
     * @param src src NPU ID of the sim_send() call
     * @param dest dest NPU ID of the sim_send() call
     * @param chunk_size chunk size of the sim_send() call
     * @return unique sim_send() chunk id
     */
    [[nodiscard]] int create_send_chunk_id(int tag,
                                           int src,
                                           int dest,
                                           ChunkSize chunk_size) noexcept;

    /**
     * Create unique chunk id for sim_recv call.
     *
     * @param tag tag of the sim_recv call
     * @param src src NPU ID of the sim_recv call
     * @param dest dest NPU ID of the sim_recv call
     * @param chunk_size chunk size of the sim_recv call
     * @return unique sim_recv chunk id
     */
    [[nodiscard]] int create_recv_chunk_id(int tag,
                                           int src,
                                           int dest,
                                           ChunkSize chunk_size) noexcept;

    /**
     * Record one fully-retired chunk for the key and erase the entry once
     * all its chunks retired with balanced send/recv counts. Without this
     * the map gains one entry per distinct (tag, src, dest, chunk_size) for
     * the life of the process -- multi-GB over a 50k-job run.
     *
     * @param tag tag of the retired chunk
     * @param src src NPU ID of the retired chunk
     * @param dest dest NPU ID of the retired chunk
     * @param chunk_size chunk size of the retired chunk
     */
    void retire_chunk(int tag,
                      int src,
                      int dest,
                      ChunkSize chunk_size) noexcept;

  private:
    /// map from (tag, src, dest, chunk_size) tuple to ChunkIdGeneratorEntry
    std::unordered_map<Key, ChunkIdGeneratorEntry, KeyHash> chunk_id_map;
};

}  // namespace AstraSimAnalytical
