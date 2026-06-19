/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/EventQueue.h"
#include "reconfigurable/Chunk.h"
#include "reconfigurable/Device.h"
#include "reconfigurable/Link.h"
#include "reconfigurable/Topology.h"
#include "reconfigurable/Type.h"
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace NetworkAnalytical;

namespace NetworkAnalyticalReconfigurable {

/**
 * Topology abstracts a network topology.
 */
class TopologyManager {
  public:
    /**
     * Constructor.
     */
    TopologyManager(int npus_count,
                    int devices_count,
                    EventQueue* event_queue,
                    std::map<int, std::vector<std::vector<Bandwidth>>> bw_schedules,
                    std::map<int, std::vector<std::vector<Latency>>> latency_schedules,
                    std::vector<int> npus_per_dim = {},
                    bool is_torus = false) noexcept;

    std::shared_ptr<Device> get_device(const DeviceId deviceId) noexcept;

    /**
     * Reconfigure the topology with new bandwidths and latencies.
     */
    bool reconfigure(std::vector<std::vector<Bandwidth>> bandwidths,
                     std::vector<std::vector<Latency>> latencies,
                     Latency reconfig_time,
                     int topo_id = 0) noexcept;

    bool reconfigure(int topo_id) noexcept;

    // Scoped per-job rewire used by rfold: set bw/lt cells for the given OCS
    // edges, install the given id-sequence routes (each: [src, ..., dst]) into
    // the route table, and push the affected devices via Device::reconfigure.
    // No global drain, no precomputeRoutes rebuild, no DOR.
    void apply_job_wiring(const std::vector<std::pair<int, int>>& ocs_edges,
                          const std::vector<std::vector<int>>& routes,
                          Bandwidth link_bw,
                          Latency link_lt) noexcept;

    // Test/inspection accessors.
    [[nodiscard]] const Route& get_precomputed_route(DeviceId src, DeviceId dst) const noexcept;
    [[nodiscard]] Bandwidth get_cell_bandwidth(DeviceId u, DeviceId v) const noexcept;

    // Mark NPUs as failed. precomputeRoutes_DOR() routes around them. Call
    // before reconfigure()/precomputeRoutes_DOR(); empty by default.
    void set_failed_npus(const std::unordered_set<int>& failed) noexcept;

    void set_reconfig_latency(Latency latency) noexcept;

    void precomputeRoutes() noexcept;

    void precomputeSingleRoute(DeviceId src, DeviceId dst) noexcept;

    /**
     * Set the logical topology dimensions for DOR routing.
     *
     * Devices are laid out in a linearized row-major fashion where dim 0 (X)
     * is the fastest-changing dimension:
     *   device_id = x + y * Nx + z * Nx * Ny + ...
     *
     * @param npus_per_dim   number of NPUs along each dimension, e.g. {Nx, Ny, Nz}
     * @param is_torus       true for torus (wrap-around links), false for mesh
     * @param bidi           when true, torus routing picks the shorter arc on
     *                       each dimension; when false (default) routing always
     *                       proceeds in the id-increasing (+1) direction.
     */
    void set_topology_dims(const std::vector<int>& npus_per_dim, bool is_torus, bool bidi) noexcept;

    /**
     * Populate precomputed_routes using Dimension-Order Routing (DOR).
     *
     * Routes always traverse dimensions in order: X first, then Y, then Z
     * (i.e., dim 0, dim 1, ..., dim N-1).
     *
     * For torus topologies:
     *   - If dor_shorter_path is false (default), routing always traverses in
     *     the id-increasing (+1) direction regardless of path length.
     *   - If dor_shorter_path is true, the shorter arc on each dimension is chosen.
     *
     * set_topology_dims() must be called at least once before this method.
     */
    void precomputeRoutes_DOR() noexcept;

    void drain_network() noexcept;

    void increment_callback() noexcept;

    void set_cur_topo_id(int topo_id) noexcept {
        cur_topo_id = topo_id;
        return;
    };

    int inflight_coll;

    /**
     * Construct the route from src to dest.
     * Route is a list of devices (pointers) that the chunk should traverse,
     * including the src and dest devices themselves.
     *
     * e.g., route(0, 3) = [0, 5, 7, 2, 3]
     *
     * @param src src NPU id
     * @param dest dest NPU id
     *
     * @return route from src NPU to dest NPU
     */
    [[nodiscard]] Route route(DeviceId src, DeviceId dest) const noexcept;

    /**
     * Initiate a transmission of a chunk.
     *
     * @param chunk chunk to be transmitted
     */
    void send(std::unique_ptr<Chunk> chunk) noexcept;

    /**
     * Get the number of NPUs in the topology.
     * NPU excludes non-NPU devices such as switches.
     *
     * @return number of NPUs in the topology
     */
    [[nodiscard]] int get_npus_count() const noexcept;

    /**
     * Get the number of devices in the topology.
     * Device includes non-NPU devices such as switches.
     *
     * @return number of devices in the topology
     */
    [[nodiscard]] int get_devices_count() const noexcept;

    bool is_reconfiguring() const noexcept;

  protected:
    /// number of total devices in the topology
    /// device includes non-NPU devices such as switches
    int devices_count;

    /// event queue to be used by the topology
    EventQueue* event_queue;

    /// number of NPUs in the topology
    /// NPU excludes non-NPU devices such as switches
    int npus_count;

    int topology_iteration;

    int cur_topo_id = -1;

    bool reconfiguring;

    Latency reconfig_time;

    /// holds the entire topology
    std::shared_ptr<Topology> topology;

    /// bandwidth matrix
    std::vector<std::vector<Bandwidth>> bandwidths;

    /// latency matrix
    std::vector<std::vector<Latency>> latencies;

    std::vector<std::vector<Route>> precomputed_routes;

    std::map<int, std::vector<std::vector<Bandwidth>>> bw_schedules;
    std::map<int, std::vector<std::vector<Latency>>> latency_schedules;

    // --- DOR routing configuration ---
    /// Number of logical dimensions (0 if not set; DOR is disabled)
    int dims_count = 0;

    /// NPUs per dimension.  dim 0 (X) is the fastest-changing index:
    ///   device_id = x + y*Nx + z*Nx*Ny + ...
    std::vector<int> npus_per_dim;

    /// Whether the topology has wrap-around links (torus) or not (mesh)
    bool is_torus = false;

    /// When true, precomputeRoutes_DOR() is used instead of the default BFS.
    /// Set automatically by set_topology_dims(); can also be toggled directly.
    bool use_dor = false;

    /// When true (bidirectional), torus DOR picks the shorter arc on each dimension.
    /// When false (default), DOR always routes in the id-increasing (+1) direction.
    bool bidi = false;

    /// NPUs marked failed: never a route endpoint or transit hop.
    std::unordered_set<int> failed_npus;
};

}  // namespace NetworkAnalyticalReconfigurable
