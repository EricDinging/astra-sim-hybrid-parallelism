/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/EventQueue.h"
#include "reconfigurable/Chunk.h"
#include "reconfigurable/Device.h"
#include "reconfigurable/FlowEngine.h"
#include "reconfigurable/Link.h"
#include "reconfigurable/Router.h"
#include "reconfigurable/Topology.h"
#include "reconfigurable/Type.h"
#include <cstddef>
#include <memory>
#include <unordered_map>
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
                    std::map<int, std::vector<BandwidthRow>> bw_schedules,
                    std::map<int, std::vector<LatencyRow>> latency_schedules,
                    std::vector<int> npus_per_dim = {},
                    bool is_torus = false,
                    bool bidi = false,
                    bool fullmesh = false) noexcept;

    // set_fluid_mode(true) installs closures into Device's process-wide
    // static hooks that capture `this`; without a destructor, a destroyed
    // TopologyManager leaves those closures dangling and the next
    // link_become_free invokes them (use-after-free).
    ~TopologyManager() noexcept;

    std::shared_ptr<Device> get_device(const DeviceId deviceId) noexcept;

    /**
     * Reconfigure the topology with new bandwidths and latencies.
     */
    bool reconfigure(std::vector<BandwidthRow> bandwidths,
                     std::vector<LatencyRow> latencies,
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

    // Mirror of apply_job_wiring, called when the owning job completes: erase
    // the (src, dst) route overrides, zero the OCS edges' bw/lt cells, and
    // disconnect the OCS links. Torus-adjacent edge pairs are skipped (they
    // never got an OCS link). Deliberately does NOT go through
    // Device::reconfigure: no iteration bumps, no link-free events. DOR mode
    // only; a warning is logged (once) in BFS mode.
    void remove_job_wiring(const std::vector<std::pair<int, int>>& ocs_edges,
                           const std::vector<std::vector<int>>& routes) noexcept;

    // Test/inspection accessors.
    [[nodiscard]] const Route& get_precomputed_route(DeviceId src, DeviceId dst) const noexcept;

    // Mark NPUs as failed; the on-demand DOR router routes around them and its
    // route cache is cleared so the change takes effect on the next lookup.
    // Empty by default.
    void set_failed_npus(const std::unordered_set<int>& failed) noexcept;

    void set_reconfig_latency(Latency latency) noexcept;

    /// Set the DOR route-cache memory ceiling (bytes). Forwarded to the Router.
    /// Default Router::kDefaultBudgetBytes (100 GiB). No-op in BFS mode.
    void set_route_cache_budget_bytes(std::size_t bytes) noexcept;

    void precomputeRoutes(int topo_id) noexcept;

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
     * Treat the network as a fully connected mesh: requires a single-topology
     * schedule (topo 0), validates that every off-diagonal BW cell is nonzero
     * (so the direct 1-hop route IS the BFS shortest path), builds a
     * direct-route Router, and defers link creation to first traffic
     * (ensure_mesh_link) instead of materializing all N^2 links up front.
     * The parsed sparse-map matrices are compressed into a scalar (uniform
     * mesh) or dense N x N store and then freed -- they are the dominant
     * fullmesh memory otherwise.
     */
    void set_fullmesh() noexcept;

    // DOR routing is computed on demand by `router_` (see Router): the eager
    // all-pairs precomputeRoutes_DOR() table was replaced by Router::compute_dor
    // + a bounded LRU cache. set_topology_dims() builds the router.

    void drain_network() noexcept;

    void increment_callback() noexcept;

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

    bool is_reconfiguring() const noexcept;

    /// Select the congestion model. Must be called after construction and
    /// before the first send. true = fluid (FlowEngine); false = serial
    /// store-and-forward (default, byte-identical to the historical path).
    void set_fluid_mode(bool enable) noexcept;

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

    // DOR-mode on-demand router (built by set_topology_dims). Declared after
    // `topology` so it is destroyed first. Null in BFS mode.
    std::unique_ptr<Router> router_;
    std::size_t route_cache_budget_bytes_ = Router::kDefaultBudgetBytes;

    // Number of directed, non-self links to drain before a reconfigure
    // completes (recomputed each drain_network()). Sparse-connectivity
    // replacement for the old all-pairs devices_count*(devices_count-1).
    int drain_target_ = 0;

    std::vector<std::vector<Route>> precomputed_routes;

    std::map<int, std::vector<BandwidthRow>> bw_schedules;
    std::map<int, std::vector<LatencyRow>> latency_schedules;

    // --- DOR routing configuration ---
    /// Number of logical dimensions (0 if not set; DOR is disabled)
    int dims_count = 0;

    /// NPUs per dimension.  dim 0 (X) is the fastest-changing index:
    ///   device_id = x + y*Nx + z*Nx*Ny + ...
    std::vector<int> npus_per_dim;

    /// Whether the topology has wrap-around links (torus) or not (mesh)
    bool is_torus = false;

    /// When true, on-demand DOR routing (router_) is used instead of the
    /// default BFS. Set automatically by set_topology_dims().
    bool use_dor = false;

    /// When true (bidirectional), torus DOR picks the shorter arc on each dimension.
    /// When false (default), DOR always routes in the id-increasing (+1) direction.
    bool bidi = false;

    /// Fully-connected-mesh mode (set_fullmesh()): direct 1-hop routes, links
    /// created lazily on first traffic.
    bool fullmesh_ = false;

    /// Fullmesh: create the (src, dest) link pair on first use, with bw/lt
    /// taken from the compressed schedule store below.
    void ensure_mesh_link(DeviceId src, DeviceId dest) noexcept;

    /// Fullmesh compressed schedule (single topo 0), built by set_fullmesh()
    /// which then frees the parsed sparse-map matrices. Uniform matrices (the
    /// gen_matrices.py case) collapse to one scalar; otherwise dense N x N.
    bool mesh_bw_uniform_ = false;
    bool mesh_lt_uniform_ = false;
    Bandwidth mesh_uniform_bw_ = Bandwidth(0);
    Latency mesh_uniform_lt_ = Latency(0);
    std::vector<std::vector<Bandwidth>> mesh_bw_;
    std::vector<std::vector<Latency>> mesh_lt_;

    /// NPUs marked failed: never a route endpoint or transit hop.
    std::unordered_set<int> failed_npus;

    /// Fluid congestion model engine (used only when fluid_ is true).
    std::unique_ptr<FlowEngine> flow_engine_;
    bool fluid_ = false;
};

}  // namespace NetworkAnalyticalReconfigurable
