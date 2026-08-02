/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "reconfigurable/Chunk.h"
#include "reconfigurable/Device.h"
#include "reconfigurable/Link.h"
#include <cassert>
#include <optional>
#include <stdio.h>

using namespace NetworkAnalyticalReconfigurable;

void Chunk::chunk_arrived_next_device(void* const chunk_ptr) noexcept {
    assert(chunk_ptr != nullptr);

    // cast to unique_ptr<Chunk>
    auto chunk = std::unique_ptr<Chunk>(static_cast<Chunk*>(chunk_ptr));

    // mark chunk arrived next node
    chunk->mark_arrived_next_device();

    if (chunk->arrived_dest()) {
        // chunk arrived dest, invoke callback
        // as chunk is unique_ptr, will be destroyed automatically
        // printf("ABCDEFGHIJ\n");
        chunk->invoke_callback();
    } else {
        // send this chunk to next dest
        const auto current_node = chunk->current_device();
        current_node->send(std::move(chunk));  // send chunk to next des
    }
}

Chunk::Chunk(const ChunkSize chunk_size,
             RoutePtr route,
             const Callback callback,
             const CallbackArg callback_arg,
             int topology_iteration) noexcept
    : route(std::move(route)),
      cursor(0),
      chunk_size(chunk_size),
      callback(callback),
      callback_arg(callback_arg),
      topology_iteration(topology_iteration) {
    assert(chunk_size > 0);
    assert(callback != nullptr);
    assert(this->route != nullptr);
}

std::shared_ptr<Device> Chunk::current_device() const noexcept {
    // assert the cursor still points into the route
    assert(cursor < route->size());

    // return the current npu in route
    return (*route)[cursor];
}

std::shared_ptr<Device> Chunk::next_device() const noexcept {
    // assert the chunk has next dest
    assert(!arrived_dest());

    // return next dest
    return (*route)[cursor + 1];
}

void Chunk::mark_arrived_next_device() noexcept {
    // if this method is being called,
    // it means the chunk hasn't arrived its final dest yet
    assert(!arrived_dest());

    // advance the cursor, marking the current node has been changed
    cursor++;
}

bool Chunk::arrived_dest() const noexcept {
    // if a chunk arrived dest, only the dest node remains ahead of the cursor
    return cursor + 1 == route->size();
}

ChunkSize Chunk::get_size() const noexcept {
    assert(chunk_size > 0);

    // return chunk size
    return chunk_size;
}

void Chunk::invoke_callback() noexcept {
    // invoke callback
    (*callback)(callback_arg);
}
