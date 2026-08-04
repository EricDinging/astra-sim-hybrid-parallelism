/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/CallbackTracker.hh"
#include <cassert>

using namespace AstraSimAnalytical;

CallbackTracker::CallbackTracker() noexcept {
    // initialize tracker
    tracker = {};
}

std::pair<CallbackTracker::Iterator, bool> CallbackTracker::
    find_or_create_entry(const int tag,
                         const int src,
                         const int dest,
                         const ChunkSize chunk_size,
                         const int chunk_id) noexcept {
    assert(tag >= 0);
    assert(src >= 0);
    assert(dest >= 0);
    assert(chunk_size > 0);
    assert(chunk_id >= 0);

    // single probe: default-construct the entry in place if absent
    const auto key = std::make_tuple(tag, src, dest, chunk_size, chunk_id);
    const auto [entry, inserted] = tracker.try_emplace(key);

    return {entry, !inserted};
}

void CallbackTracker::erase_entry(const Iterator entry) noexcept {
    assert(entry != tracker.end());

    // erase by iterator: no key re-hash
    tracker.erase(entry);
}

void CallbackTracker::pop_entry(const int tag,
                                const int src,
                                const int dest,
                                const ChunkSize chunk_size,
                                const int chunk_id) noexcept {
    assert(tag >= 0);
    assert(src >= 0);
    assert(dest >= 0);
    assert(chunk_size > 0);
    assert(chunk_id >= 0);

    // create key
    const auto key = std::make_tuple(tag, src, dest, chunk_size, chunk_id);

    // find entry
    const auto entry = tracker.find(key);
    assert(entry != tracker.end());  // entry must exist

    // erase entry from the tracker
    tracker.erase(entry);
}
