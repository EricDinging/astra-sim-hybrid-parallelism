/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <memory>
#include <optional>
#include <vector>

namespace NetworkAnalyticalReconfigurable {

/// Forward declarations of network components
class Chunk;
class Link;
class Device;

/// Route is a sequence of devices
using Route = std::vector<std::shared_ptr<Device>>;

/// Shared, immutable route handle. The Router cache and every in-flight Chunk
/// share one Route instance (one refcount bump per chunk instead of a per-chunk
/// deep copy of the whole hop list); chunks track progress with a cursor.
using RoutePtr = std::shared_ptr<const Route>;

}  // namespace NetworkAnalyticalReconfigurable
