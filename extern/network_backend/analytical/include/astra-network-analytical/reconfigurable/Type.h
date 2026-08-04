/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace NetworkAnalyticalReconfigurable {

/// Forward declarations of network components
class Chunk;
class Link;
class Device;

/// Route is a sequence of devices, plus a per-hop port-index side-cache used
/// by Device::send to skip the port-table lower_bound on the hot path.
/// Deriving from the vector keeps every existing construction/iteration site
/// (push_back, indexing, back(), initializer lists) source-compatible.
///
/// hop_ports[c] caches the index into device hops[c]'s port table of the port
/// toward hops[c+1] (kNoPort = unresolved). Entries are valid only while
/// links_epoch matches Device::wiring_epoch, which is bumped on every
/// connect/disconnect/reconfigure -- on mismatch the whole cache is refilled
/// with kNoPort and restamped, so stale indices are unreachable. Chunks share
/// one Route instance concurrently (RoutePtr from the Router cache); the
/// simulator is single-threaded, so mutable cache writes through the shared
/// const handle are safe.
struct Route : std::vector<std::shared_ptr<Device>> {
    using std::vector<std::shared_ptr<Device>>::vector;

    /// sentinel for "port index not resolved yet"
    static constexpr std::uint32_t kNoPort = 0xFFFFFFFFU;

    mutable std::vector<std::uint32_t> hop_ports;
    mutable std::uint64_t links_epoch = 0;
};

/// Shared, immutable route handle. The Router cache and every in-flight Chunk
/// share one Route instance (one refcount bump per chunk instead of a per-chunk
/// deep copy of the whole hop list); chunks track progress with a cursor.
using RoutePtr = std::shared_ptr<const Route>;

}  // namespace NetworkAnalyticalReconfigurable
