/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/ChunkIdGenerator.hh"
#include <cassert>

using namespace AstraSimAnalytical;

ChunkIdGenerator::ChunkIdGenerator() noexcept {
    chunk_id_map = {};
}

std::pair<int, ChunkIdGeneratorEntry*> ChunkIdGenerator::create_send_chunk_id(
    const int tag,
    const int src,
    const int dest,
    const ChunkSize chunk_size) noexcept {
    assert(tag >= 0);
    assert(src >= 0);
    assert(dest >= 0);
    assert(chunk_size > 0);

    // create key
    const auto key = std::make_tuple(tag, src, dest, chunk_size);

    // single probe: default-construct the entry in place if absent
    const auto entry = chunk_id_map.try_emplace(key).first;

    // increment id and return
    entry->second.increment_send_id();
    return {entry->second.get_send_id(), &(entry->second)};
}

std::pair<int, ChunkIdGeneratorEntry*> ChunkIdGenerator::create_recv_chunk_id(
    const int tag,
    const int src,
    const int dest,
    const ChunkSize chunk_size) noexcept {
    assert(tag >= 0);
    assert(src >= 0);
    assert(dest >= 0);
    assert(chunk_size > 0);

    // create key
    const auto key = std::make_tuple(tag, src, dest, chunk_size);

    // single probe: default-construct the entry in place if absent
    const auto entry = chunk_id_map.try_emplace(key).first;

    // increment id and return
    entry->second.increment_recv_id();
    return {entry->second.get_recv_id(), &(entry->second)};
}

void ChunkIdGenerator::retire_chunk(ChunkIdGeneratorEntry* const entry,
                                    const int tag,
                                    const int src,
                                    const int dest,
                                    const ChunkSize chunk_size) noexcept {
    assert(entry != nullptr);

    entry->increment_completed();
    if (entry->fully_retired()) {
        // sole keyed probe on the retire path: erase only on the
        // fully-retired transition
        const auto key = std::make_tuple(tag, src, dest, chunk_size);
        [[maybe_unused]] const auto erased = chunk_id_map.erase(key);
        assert(erased == 1);
    }
}
