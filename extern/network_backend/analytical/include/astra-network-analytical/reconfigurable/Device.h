/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/Type.h"
#include "reconfigurable/Type.h"
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace NetworkAnalytical;

namespace NetworkAnalyticalReconfigurable {

class Router;  // Forward declaration (DOR-mode on-demand routing)

// class TopologyManager; // Forward declaration

/// Sparse per-device bandwidth/latency row: absent key == 0. Dense N-wide
/// vectors are ~99% zero for a torus (~6N real links), so rows are stored by
/// link id instead.
using BandwidthRow = std::unordered_map<DeviceId, Bandwidth>;
using LatencyRow = std::unordered_map<DeviceId, Latency>;

/**
 * Device class represents a single device in the network.
 * Device is usually an NPU or a switch.
 */
class Device : public std::enable_shared_from_this<Device> {
  public:
    /// One outgoing port: the link plus its pending-chunk queue. The two were
    /// twin maps with identical key sets maintained in lockstep; merging them
    /// makes every per-hop lookup a single search instead of two.
    struct Port {
        std::shared_ptr<Link> link;
        std::list<std::unique_ptr<Chunk>> pending;
    };

    /// Sorted-by-DeviceId port table (~7-8 entries per device). Ascending
    /// iteration order matches the std::map it replaced -- same-tick event
    /// insertion order in drain/reconfigure depends on it.
    using PortVec = std::vector<std::pair<DeviceId, Port>>;

    /**
     * Constructor.
     *
     * @param id id of the device
     */
    explicit Device(DeviceId id) noexcept;

    static std::function<void()> increment_callback;  // Callback to be invoked when a link becomes free

    /// Fluid mode: notifies the FlowEngine that a link finished its
    /// reconfig-downtime window (link_become_free). Default no-op.
    static std::function<void(Link*)> link_freed_hook;

    /**
     * Get id of the device.
     *
     * @return id of the device
     */
    [[nodiscard]] DeviceId get_id() const noexcept;

    /**
     * Callback to be called when a link becomes free.
     *  - If the link has pending chunks, process the first one.
     *  - If the link has no pending chunks, set the link as free.
     *
     * @param link_ptr pointer to the link that becomes free
     */
    static void link_become_free(void* const arg) noexcept;

    void link_become_free(DeviceId link_id) noexcept;

    /**
     * Initiate a chunk transmission.
     * You must invoke this method on the source device of the chunk.
     *
     * @param chunk chunk to send
     */
    void send(std::unique_ptr<Chunk> chunk) noexcept;

    /**
     * Connect a device to another device.
     *
     * @param id id of the device to connect this device to
     * @param bandwidth bandwidth of the link
     * @param latency latency of the link
     */
    void connect(DeviceId id, Bandwidth bandwidth, Latency latency) noexcept;

    // Rows are taken by const reference: they are sparse per-device maps
    // (absent == 0) and a global reconfigure calls this once per device.
    //
    // scoped = per-job wiring (apply_job_wiring): only links whose
    // bandwidth/latency actually changed are touched, and topology_iteration
    // is NOT bumped -- in-flight chunks routed on the base topology stay
    // valid, and unchanged links keep their busy state and pending queues
    // (bumping marked every transiting chunk stale so the refresh truncated
    // its route, P0-1; the unconditional +1ns free events force-freed busy
    // links, P0-2). Global reconfigure (scoped = false, post-drain) keeps the
    // original bump-everything behavior.
    void reconfigure(const BandwidthRow& bandwidths,
                     const std::vector<Route>& routes,
                     const LatencyRow& latencies,
                     Latency reconfigTime,
                     bool scoped = false) noexcept;

    /// In DOR mode the device fetches routes on demand from this Router instead
    /// of holding a per-row copy. nullptr (BFS mode) falls back to `routes`.
    void set_router(Router* router) noexcept {
        router_ = router;
    }

    /**
     * Disconnect a device from another device.
     *
     * @param id id of the device to disconnect this device from
     */
    void disconnect(DeviceId id) noexcept;

    int pending_chunks_count(DeviceId id) const noexcept;

    std::shared_ptr<Link> get_link(DeviceId id) const noexcept;

    /// Read-only view of this device's ports (sparse: ~6 torus neighbors + OCS
    /// edges + self-loop), sorted ascending by DeviceId. Used by the drain and
    /// reconfigure-teardown passes.
    const PortVec& ports() const noexcept {
        return ports_;
    }

    /**
     * Check if this device is connected to another device.
     *
     * @param dest id of the device to check the connectivity
     * @return true if connected to the given device, false otherwise
     */
    [[nodiscard]] bool connected(DeviceId dest) const noexcept;

    /// Global wiring epoch: bumped by every operation that can change a
    /// device->link mapping or a link's identity (connect, disconnect,
    /// reconfigure). Route::hop_ports entries are valid only under a matching
    /// epoch (see Route in reconfigurable/Type.h).
    static std::uint64_t wiring_epoch;

  private:
    /// device Id
    DeviceId device_id;

    int topology_iteration;

    /// ports to other nodes, sorted by dest DeviceId (link + pending queue
    /// merged; see Port). Lookup = std::lower_bound; connect/disconnect are
    /// O(n) vector insert/erase but wiring-time only.
    PortVec ports_;

    /// sorted-insert position / lookup helpers for ports_
    [[nodiscard]] PortVec::iterator port_pos(DeviceId id) noexcept;
    [[nodiscard]] PortVec::const_iterator port_pos(DeviceId id) const noexcept;
    [[nodiscard]] Port* find_port(DeviceId id) noexcept;
    [[nodiscard]] const Port* find_port(DeviceId id) const noexcept;

    std::map<DeviceId, RoutePtr> routes;  // BFS mode only; empty under DOR

    // DOR-mode on-demand router (non-owning). nullptr => BFS mode (use `routes`).
    Router* router_ = nullptr;
};

}  // namespace NetworkAnalyticalReconfigurable
