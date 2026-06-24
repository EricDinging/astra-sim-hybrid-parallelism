/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "reconfigurable/Topology.h"
#include "reconfigurable/Type.h"

namespace NetworkAnalyticalReconfigurable {

/**
 * Router computes Dimension-Order-Routing (DOR) routes on demand instead of
 * materializing the full N x N table. DOR is purely geometric (it never reads
 * the bandwidth matrix), so any route can be recomputed from coordinates; only
 * the OCS overrides installed by TopologyManager::apply_job_wiring() are
 * non-geometric and stored explicitly (sparsely).
 *
 * lookup(src, dst) returns, in priority order: an installed override, a cached
 * route, or a freshly computed DOR route (inserted into a bounded LRU cache).
 * The result is bit-identical to the value the eager precomputeRoutes_DOR()
 * table held -- compute_dor() is the original per-pair walk lifted verbatim,
 * and the override/cache lifecycle mirrors the eager table's state machine
 * (clear() == rebuild-to-DOR, set_override() == apply_job_wiring overwrite).
 */
class Router {
  public:
    /// 100 GiB default cache ceiling (a safety bound, not a target; the live
    /// footprint is the working set of active comm pairs, typically far less).
    static constexpr std::size_t kDefaultBudgetBytes = static_cast<std::size_t>(100) * 1024 * 1024 * 1024;

    Router(std::shared_ptr<Topology> topology,
           std::vector<int> npus_per_dim,
           bool is_torus,
           bool bidi,
           int devices_count,
           const std::unordered_set<int>* failed_npus,
           std::size_t budget_bytes = kDefaultBudgetBytes) noexcept;

    /**
     * Route from src to dst (override > cache > computed DOR).
     *
     * The returned reference is valid until the next lookup() that misses and
     * evicts; every caller copies it into the Chunk immediately (Chunk owns its
     * route, copied by value) before any other lookup, and the simulator is
     * single-threaded, so no eviction can dangle the reference. Overrides are
     * never evicted.
     */
    const Route& lookup(DeviceId src, DeviceId dst) const noexcept;

    /// Install an OCS override for (s, t) and drop any stale cached entry.
    void set_override(DeviceId s, DeviceId t, Route route) noexcept;

    /// Drop all overrides and cached routes (== a full reconfigure to pure DOR).
    void clear() noexcept;

    /// Drop cached routes only (e.g. when the failed-node set changes).
    void clear_cache() const noexcept;

    void set_budget_bytes(std::size_t bytes) noexcept;

    /// The original DOR walk, lifted verbatim from precomputeRoutes_DOR().
    Route compute_dor(DeviceId src, DeviceId dst) const noexcept;

    // Introspection (tests / diagnostics).
    std::size_t cache_size() const noexcept {
        return cache_.size();
    }
    std::size_t cache_bytes() const noexcept {
        return cur_bytes_;
    }
    std::size_t override_count() const noexcept {
        return overrides_.size();
    }

  private:
    std::uint64_t key(DeviceId s, DeviceId t) const noexcept {
        return static_cast<std::uint64_t>(s) * static_cast<std::uint64_t>(devices_count_) +
               static_cast<std::uint64_t>(t);
    }

    /// Approximate resident bytes of a cached route (list node + shared_ptr +
    /// allocator overhead per hop, plus the LRU/map bookkeeping per entry).
    static std::size_t route_bytes(const Route& r) noexcept {
        constexpr std::size_t kPerHop = 56;    // list node + shared_ptr + malloc hdr
        constexpr std::size_t kPerEntry = 96;  // list pair + 2 unordered_map nodes
        return kPerEntry + kPerHop * r.size();
    }

    void invalidate(DeviceId s, DeviceId t) const noexcept;
    void evict_to_budget() const noexcept;

    std::shared_ptr<Topology> topology_;
    std::vector<int> npus_per_dim_;
    int dims_count_;
    bool is_torus_;
    bool bidi_;
    int devices_count_;
    const std::unordered_set<int>* failed_npus_;

    // Sparse OCS overrides. Persistent, consulted first, never evicted.
    std::unordered_map<std::uint64_t, Route> overrides_;

    // Bounded LRU cache: front == most-recently-used. The cache is logically
    // const (a pure accelerator), hence mutable.
    using LruList = std::list<std::pair<std::uint64_t, Route>>;
    mutable LruList lru_;
    mutable std::unordered_map<std::uint64_t, LruList::iterator> cache_;
    mutable std::size_t cur_bytes_;
    std::size_t budget_bytes_;

    // DOR-walk scratch, reused across calls (one O(N) alloc, not per lookup).
    mutable std::vector<long long> visited_;
    mutable long long epoch_;
};

}  // namespace NetworkAnalyticalReconfigurable
