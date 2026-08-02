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

int ChunkIdGenerator::create_send_chunk_id(
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
    return entry->second.get_send_id();
}

int ChunkIdGenerator::create_recv_chunk_id(
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
    return entry->second.get_recv_id();
}

void ChunkIdGenerator::retire_chunk(const int tag,
                                    const int src,
                                    const int dest,
                                    const ChunkSize chunk_size) noexcept {
    const auto key = std::make_tuple(tag, src, dest, chunk_size);
    const auto entry = chunk_id_map.find(key);
    assert(entry != chunk_id_map.end());

    entry->second.increment_completed();
    if (entry->second.fully_retired()) {
        chunk_id_map.erase(entry);
    }
}
