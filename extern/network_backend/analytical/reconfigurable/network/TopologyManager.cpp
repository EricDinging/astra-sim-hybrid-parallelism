#include "reconfigurable/TopologyManager.h"
#include "common/HelperFunction.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <set>

using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

TopologyManager::TopologyManager(int npus_count,
                                 int devices_count,
                                 EventQueue* event_queue,
                                 std::map<int, std::vector<BandwidthRow>> bw_schedules,
                                 std::map<int, std::vector<LatencyRow>> latency_schedules,
                                 std::vector<int> npus_per_dim,
                                 bool is_torus,
                                 bool bidi,
                                 bool fullmesh) noexcept {
    this->npus_count = npus_count;
    this->devices_count = devices_count;
    this->event_queue = event_queue;
    flow_engine_ = std::make_unique<FlowEngine>(event_queue);
    this->bw_schedules = std::move(bw_schedules);
    this->latency_schedules = std::move(latency_schedules);
    debug_print("BW schedules size: " + std::to_string(this->bw_schedules.size()));
    debug_print("LT schedules size: " + std::to_string(this->latency_schedules.size()));

    assert(npus_count > 0);
    assert(devices_count > 0);
    assert(devices_count >= npus_count);

    reconfiguring = false;

    topology = std::make_shared<Topology>(npus_count, devices_count);

    Link::increment_callback = [this]() noexcept { this->increment_callback(); };

    Device::increment_callback = [this]() noexcept { this->increment_callback(); };

    topology_iteration = 0;
    inflight_coll = 0;
    reconfig_time = Latency(0);

    if (fullmesh) {
        // Fully connected mesh: direct 1-hop routes, lazy link creation.
        // npus_per_dim (if any) is a scheduler-side notion and is ignored here.
        set_fullmesh();
    } else if (!npus_per_dim.empty()) {
        // Baseline DOR arc selection is unidirectional (+1 arc per axis) by
        // default; pass bidi=true to pick the shorter arc per dimension.
        // Mapping-aware placement policies (folding/rfold) install their own
        // per-job routes via apply_job_wiring(), which override this table for
        // their ring edges; everything else (firstfit, scatter baselines)
        // routes here.
        set_topology_dims(npus_per_dim, is_torus, bidi);
    } else {
        // BFS mode (no geometry): fall back to all-pairs connectivity. Only
        // used for small non-torus / test topologies.
        topology->connect_all_pairs();
    }
}

TopologyManager::~TopologyManager() noexcept {
    // Unconditional reset: the hooks captured `this` iff set_fluid_mode(true)
    // was ever called, but resetting unconditionally is cheap and correct
    // either way, and a dying instance's hooks must never outlive it.
    Device::link_freed_hook = [](Link*) noexcept {};
    Device::flow_count_probe = [](const Link*) noexcept { return 0; };
}

std::shared_ptr<Device> TopologyManager::get_device(const DeviceId deviceId) noexcept {
    // Validate the device ID
    assert(deviceId >= 0 && deviceId < devices_count);
    return topology->get_device(deviceId);
}

void TopologyManager::drain_network() noexcept {
    // Drain only the links that actually exist (sparse connectivity). Pass 1
    // counts the directed, non-self links so increment_callback knows when the
    // drain is complete; pass 2 reports every idle link (busy links report
    // themselves later, when they become free).
    Link::num_drained_links = 0;
    drain_target_ = 0;
    for (int i = 0; i < devices_count; ++i) {
        for (const auto& [id, link] : topology->get_device(i)->get_links()) {
            if (id != i) {
                ++drain_target_;
            }
        }
    }

    for (int i = 0; i < devices_count; ++i) {
        auto device = topology->get_device(i);
        for (const auto& [id, link] : device->get_links()) {
            if (id != i && !link->is_busy() && (!fluid_ || flow_engine_->active_flows(link.get()) == 0)) {
                increment_callback();
            }
        }
    }

    // Nothing to drain (degenerate topology): finalize immediately.
    if (drain_target_ == 0) {
        increment_callback();
    }
}

bool TopologyManager::is_reconfiguring() const noexcept {
    return reconfiguring;
}

void TopologyManager::set_fluid_mode(const bool enable) noexcept {
    fluid_ = enable;
    if (enable) {
        // Fluid mode reads link occupancy from the engine, not Link::busy.
        Device::link_freed_hook = [this](Link* link) noexcept { flow_engine_->on_link_updated(link); };
        Device::flow_count_probe = [this](const Link* link) noexcept { return flow_engine_->active_flows(link); };
    } else {
        Device::link_freed_hook = [](Link*) noexcept {};
        Device::flow_count_probe = [](const Link*) noexcept { return 0; };
    }
}

void TopologyManager::set_fluid_lazy_finish(const bool enable) noexcept {
    flow_engine_->set_lazy_finish(enable);
}

void TopologyManager::increment_callback() noexcept {
    if (!reconfiguring) {
        Link::num_drained_links = 0;
        return;
    }

    // Increment the topology iteration
    Link::num_drained_links++;

    if (Link::num_drained_links < drain_target_) {
        return;
    }

    Link::num_drained_links = 0;
    reconfiguring = false;

    // All links have been drained, increment the topology iteration
    // Message construction guarded: these loops stringify the full NxN matrix
    // (O(N^2) to_string + row copies) and must be statically dead when
    // verbose logging is compiled out.
    if (kVerboseLogging) {
        debug_print("Drained Network, reconfiguring to TOPO ITERATION #" + std::to_string(topology_iteration));
        for (const auto& row : bw_schedules.at(cur_topo_id)) {
            std::string msg;
            for (const auto& [id, bw] : row) {
                msg += std::to_string(id) + ":" + std::to_string(static_cast<int>(bw)) + " ";
            }
            debug_print(msg);
        }
    }

    for (int i = 0; i < devices_count; ++i) {
        auto device = topology->get_device(i);
        if (kVerboseLogging) {
            debug_print("BW Vector for device " + std::to_string(i) + ":");
            std::string msg;
            for (const auto& [id, bw] : bw_schedules.at(cur_topo_id)[i]) {
                msg += std::to_string(id) + ":" + std::to_string(static_cast<int>(bw)) + " ";
            }
            debug_print(msg);
        }

        // DOR mode fetches routes on demand (router_); pass an empty route row
        // so the device reconfigures links only. BFS mode pushes the full row.
        static const std::vector<Route> kNoRoutes;
        device->reconfigure(bw_schedules.at(cur_topo_id)[i], use_dor ? kNoRoutes : precomputed_routes[i],
                            latency_schedules.at(cur_topo_id)[i], reconfig_time);
    }

    if (fluid_) {
        flow_engine_->resume();
    }
}

bool TopologyManager::reconfigure(std::vector<BandwidthRow> bandwidths,
                                  std::vector<LatencyRow> latencies,
                                  Latency reconfig_time,
                                  int topo_id) noexcept {

    if (topo_id == cur_topo_id) {
        debug_print(
            "TM: Already in the requested topology and reconfiguring, ignoring reconfiguration request to topo_id " +
            std::to_string(topo_id));
        return true;
    }

    if ((is_reconfiguring() || inflight_coll > 0)) {
        // TODO check condition
        debug_print("TM: trying to reconfig, inflight coll: " + std::to_string(inflight_coll) + ", is reconfiguring? " +
                    std::to_string(is_reconfiguring()) + ", is event queue finished? " +
                    std::to_string(event_queue->finished()));
        // event_queue->proceed();
        return false;
    }

    debug_print("TM: !!! Reconfig to topo_id: " + std::to_string(topo_id) +
                ", Devices count: " + std::to_string(devices_count) + ", NPUs count: " + std::to_string(npus_count) +
                ", inflight_coll " + std::to_string(inflight_coll));
    debug_print("TM: bandwidths size: " + std::to_string(bandwidths.size()) +
                ", latencies size: " + std::to_string(latencies.size()));
    if (kVerboseLogging) {
        for (const auto& row : bandwidths) {
            std::string msg;
            for (const auto& [id, bw] : row) {
                msg += std::to_string(id) + ":" + std::to_string(static_cast<int>(bw)) + " ";
            }
            debug_print(msg);
        }
    }

    assert(bandwidths.size() == devices_count);
    assert(latencies.size() == devices_count);
    assert(!reconfiguring);

    // Register the target rows in the schedule (drives by cur_topo_id below).
    bw_schedules[topo_id] = std::move(bandwidths);
    latency_schedules[topo_id] = std::move(latencies);
    this->reconfig_time = reconfig_time;

    if (router_ != nullptr) {
        // On-demand routing (DOR or fullmesh): a full reconfigure ==
        // rebuild-to-pure-computed-routes, i.e. drop overrides + cached routes.
        router_->clear();
    } else {
        precomputeRoutes(topo_id);
    }

    reconfiguring = true;
    if (fluid_) {
        // Defer new flows for the length of the drain; resume() runs in
        // increment_callback once the post-drain retune is done.
        flow_engine_->pause();
    }
    this->cur_topo_id = topo_id;
    topology_iteration++;
    drain_network();
    return true;
}

bool TopologyManager::reconfigure(int topo_id) noexcept {
    auto bw_it = bw_schedules.find(topo_id);
    auto lt_it = latency_schedules.find(topo_id);
    if (bw_it == bw_schedules.end() || lt_it == latency_schedules.end()) {
        debug_print("Topology ID " + std::to_string(topo_id) + " not found in BW/LT schedules.");
        exit(1);
    }
    return reconfigure(bw_it->second, lt_it->second, reconfig_time, topo_id);
}

void TopologyManager::set_reconfig_latency(Latency latency) noexcept {
    this->reconfig_time = latency;
}

void TopologyManager::precomputeRoutes(int topo_id) noexcept {
    // TODO: add other routing algorithms
    // Adjacency list. Built from the just-registered target schedule row
    // (bw_schedules[topo_id]), not live link bandwidths: this runs before the
    // links are retuned to the new topology (that happens later, in
    // increment_callback after the drain), so live links are stale/zero here.
    std::vector<std::vector<int>> adj(devices_count);
    for (int i = 0; i < devices_count; ++i) {
        for (const auto& [j, bw] : bw_schedules.at(topo_id)[i]) {
            if (i != j && bw > 0) {
                adj[i].push_back(j);
            }
        }
    }

    for (auto& v : adj) {
        sort(v.begin(), v.end());
        v.erase(unique(v.begin(), v.end()), v.end());
    }

    precomputed_routes = std::vector<std::vector<Route>>(devices_count, std::vector<Route>(devices_count));

    // BFS
    const int INF = 1e9;
    std::vector<int> dist(devices_count), parent(devices_count);
    std::queue<int> q;

    for (int s = 0; s < devices_count; ++s) {
        // BFS init
        fill(dist.begin(), dist.end(), INF);
        fill(parent.begin(), parent.end(), -1);
        while (!q.empty()) {
            q.pop();
        }
        dist[s] = 0;
        q.push(s);

        // BFS
        while (!q.empty()) {
            int u = q.front();
            q.pop();
            for (int v : adj[u]) {
                if (dist[v] == INF) {
                    dist[v] = dist[u] + 1;
                    parent[v] = u;
                    q.push(v);
                }
            }
        }

        // Reconstruct a path s -> t for all t
        for (int t = 0; t < devices_count; ++t) {
            if (s == t) {
                precomputed_routes[s][t] = {topology->get_device(s)};
            } else if (parent[t] == -1) {
                precomputed_routes[s][t] = {topology->get_device(s),
                                            topology->get_device(t)};  // Unreachable, stub route
            } else {
                Route path;
                for (int cur = t; cur != -1; cur = parent[cur]) {
                    path.push_back(topology->get_device(cur));
                }
                reverse(path.begin(), path.end());
                precomputed_routes[s][t] = std::move(path);
            }
        }
    }
}

void TopologyManager::set_topology_dims(const std::vector<int>& npus_per_dim, bool is_torus, bool bidi) noexcept {
    assert(!npus_per_dim.empty());

    // Validate that the product of dims matches the number of NPUs
    int product = 1;
    for (int d : npus_per_dim) {
        assert(d > 0);
        product *= d;
    }
    assert(product == npus_count);

    this->npus_per_dim = npus_per_dim;
    this->dims_count = static_cast<int>(npus_per_dim.size());
    this->is_torus = is_torus;
    this->bidi = bidi;
    this->use_dor = true;  // enable DOR routing

    // Build the on-demand DOR router (replaces the eager all-pairs table) and
    // point every device at it. It reads failed_npus by pointer, so later
    // set_failed_npus() calls are picked up.
    router_ = std::make_unique<Router>(topology, this->npus_per_dim, this->is_torus, this->bidi, devices_count,
                                       &failed_npus, route_cache_budget_bytes_);
    for (int i = 0; i < devices_count; ++i) {
        topology->get_device(i)->set_router(router_.get());
    }

    // Sparse torus connectivity: 6 neighbor links/node instead of all-pairs.
    // Links start at zero bandwidth; reconfigure() sets the real values from
    // the schedule matrices (only neighbor + OCS cells are nonzero).
    topology->connect_torus_neighbors(this->npus_per_dim, this->is_torus);
}

void TopologyManager::set_fullmesh() noexcept {
    // Single topology only: the compressed schedule below frees the parsed
    // matrices, so there is nothing left to reconfigure to mid-run.
    if (bw_schedules.size() != 1 || bw_schedules.begin()->first != 0) {
        std::cerr << "[Error] --fullmesh supports exactly one topology (topo 0); got " << bw_schedules.size()
                  << " BW blocks" << std::endl;
        std::exit(1);
    }
    const auto& mat = bw_schedules.at(0);
    const auto& lt_mat = latency_schedules.at(0);
    if (static_cast<int>(mat.size()) != devices_count) {
        std::cerr << "[Error] --fullmesh: BW matrix has " << mat.size() << " rows, expected " << devices_count
                  << std::endl;
        std::exit(1);
    }
    assert(static_cast<int>(lt_mat.size()) == devices_count);

    // Validate every off-diagonal BW cell is nonzero: the direct-route Router
    // below is BFS-correct only on a complete graph (shortest path ==
    // [src, dst]). The same pass detects uniformity for the compressed store.
    const auto lt_of = [](const LatencyRow& row, DeviceId id) noexcept {
        const auto it = row.find(id);
        return it == row.end() ? Latency(0) : it->second;
    };
    mesh_bw_uniform_ = true;
    mesh_lt_uniform_ = true;
    mesh_uniform_bw_ = mat[0].count(1) != 0U ? mat[0].at(1) : Bandwidth(0);
    mesh_uniform_lt_ = lt_of(lt_mat[0], 1);
    for (int i = 0; i < devices_count; ++i) {
        for (int j = 0; j < devices_count; ++j) {
            if (i == j) {
                continue;
            }
            const auto it = mat[i].find(j);
            if (it == mat[i].end() || it->second <= Bandwidth(0)) {
                std::cerr << "[Error] --fullmesh: BW matrix cell (" << i << ", " << j
                          << ") is zero or missing; the input is not a full mesh" << std::endl;
                std::exit(1);
            }
            mesh_bw_uniform_ = mesh_bw_uniform_ && it->second == mesh_uniform_bw_;
            mesh_lt_uniform_ = mesh_lt_uniform_ && lt_of(lt_mat[i], j) == mesh_uniform_lt_;
        }
    }
    if (!mesh_bw_uniform_) {
        mesh_bw_.assign(devices_count, std::vector<Bandwidth>(devices_count, Bandwidth(0)));
        for (int i = 0; i < devices_count; ++i) {
            for (const auto& [j, bw] : mat[i]) {
                mesh_bw_[i][j] = bw;
            }
        }
    }
    if (!mesh_lt_uniform_) {
        mesh_lt_.assign(devices_count, std::vector<Latency>(devices_count, Latency(0)));
        for (int i = 0; i < devices_count; ++i) {
            for (const auto& [j, lt] : lt_mat[i]) {
                mesh_lt_[i][j] = lt;
            }
        }
    }

    // The compressed store above is all ensure_mesh_link() needs; free the
    // parsed sparse-map matrices (O(N^2) map nodes -- the dominant fullmesh
    // footprint) and leave empty placeholder rows so the initial
    // reconfigure(0) -> increment_callback flow runs unchanged. Empty rows
    // are exact there: no non-self links exist before first traffic, and
    // Device::reconfigure only touches existing links.
    bw_schedules.clear();
    latency_schedules.clear();
    bw_schedules[0] = std::vector<BandwidthRow>(devices_count);
    latency_schedules[0] = std::vector<LatencyRow>(devices_count);

    fullmesh_ = true;
    use_dor = true;  // on-demand routing; never build the eager N^2 route tables

    router_ = std::make_unique<Router>(topology, std::vector<int>{}, /*is_torus=*/false, /*bidi=*/false, devices_count,
                                       &failed_npus, route_cache_budget_bytes_, /*fullmesh=*/true);
    for (int i = 0; i < devices_count; ++i) {
        topology->get_device(i)->set_router(router_.get());
    }
    // No links are created here: a full mesh has N^2 directed links but a
    // job's ring edges touch only a handful of pairs, so links materialize on
    // first traffic (ensure_mesh_link).
}

void TopologyManager::ensure_mesh_link(DeviceId src, DeviceId dest) noexcept {
    if (src == dest || topology->get_device(src)->connected(dest)) {
        return;
    }
    const Bandwidth bw_fwd = mesh_bw_uniform_ ? mesh_uniform_bw_ : mesh_bw_[src][dest];
    const Bandwidth bw_rev = mesh_bw_uniform_ ? mesh_uniform_bw_ : mesh_bw_[dest][src];
    const Latency lt_fwd = mesh_lt_uniform_ ? mesh_uniform_lt_ : mesh_lt_[src][dest];
    const Latency lt_rev = mesh_lt_uniform_ ? mesh_uniform_lt_ : mesh_lt_[dest][src];
    topology->connect_mesh_edge(src, dest, bw_fwd, lt_fwd, bw_rev, lt_rev);
}

void TopologyManager::set_failed_npus(const std::unordered_set<int>& failed) noexcept {
    failed_npus = failed;
    // DOR routes around failed NPUs; drop any cached pre-failure routes.
    if (router_ != nullptr) {
        router_->clear_cache();
    }
}

void TopologyManager::set_route_cache_budget_bytes(std::size_t bytes) noexcept {
    route_cache_budget_bytes_ = bytes;
    if (router_ != nullptr) {
        router_->set_budget_bytes(bytes);
    }
}

void TopologyManager::send(std::unique_ptr<Chunk> chunk) noexcept {
    assert(chunk != nullptr);
    assert(chunk->current_device() != nullptr);
    // chunk->update_route(route(chunk->current_device()->get_id(), chunk->next_device()->get_id()),
    // topology_iteration);

    // Get the source device ID
    DeviceId src = chunk->current_device()->get_id();
    assert(src >= 0 && src < devices_count);

    if (chunk->get_topology_iteration() == -1) {
        DeviceId dest = chunk->next_device()->get_id();
        if (fullmesh_) {
            ensure_mesh_link(src, dest);
        }
        // DOR/fullmesh mode: fetch on demand; BFS mode: index the eager table.
        if (router_ != nullptr) {
            chunk->update_route(router_->lookup(src, dest), topology_iteration);
        } else {
            chunk->update_route(std::make_shared<const Route>(precomputed_routes[src][dest]), topology_iteration);
        }
    }

    // printf("TM: Sending chunk from %d to %d, in topo iter %d, route: ", chunk->current_device()->get_id(),
    // chunk->next_device()->get_id(), chunk->get_topology_iteration()); for(auto device : chunk->route){
    //     printf("%d ", device->get_id());
    // }
    // printf("\n");

    if (fluid_) {
        flow_engine_->start_flow(std::move(chunk));
        return;
    }

    // Send the chunk through the topology
    topology->send(std::move(chunk));
}

void TopologyManager::send(const ChunkSize chunk_size,
                           const DeviceId src,
                           const DeviceId dest,
                           const Callback callback,
                           const CallbackArg callback_arg) noexcept {
    assert(src >= 0 && src < devices_count);
    assert(dest >= 0 && dest < devices_count);

    // Same sequence as send(chunk) on a stub-routed chunk (iteration -1),
    // minus the throwaway 2-node stub route allocation.
    if (fullmesh_) {
        ensure_mesh_link(src, dest);
    }
    // DOR/fullmesh mode: fetch on demand; BFS mode: index the eager table.
    auto route = (router_ != nullptr) ? router_->lookup(src, dest)
                                      : std::make_shared<const Route>(precomputed_routes[src][dest]);
    auto chunk = std::make_unique<Chunk>(chunk_size, std::move(route), callback, callback_arg, topology_iteration);

    if (fluid_) {
        flow_engine_->start_flow(std::move(chunk));
        return;
    }

    // Send the chunk through the topology
    topology->send(std::move(chunk));
}

Route TopologyManager::route(DeviceId src, DeviceId dest) const noexcept {
    // Ensure src and dest are valid
    assert(src >= 0 && src < npus_count);
    assert(dest >= 0 && dest < npus_count);

    // Without any host forwarding.
    Route route;
    route.push_back(topology->get_device(src));

    // Create a route that includes the src and dest devices
    route.push_back(topology->get_device(dest));
    return route;
}

void TopologyManager::apply_job_wiring(const std::vector<std::pair<int, int>>& ocs_edges,
                                       const std::vector<std::vector<int>>& routes,
                                       Bandwidth link_bw,
                                       Latency link_lt) noexcept {
    // BFS mode keeps the eager table; DOR mode stores overrides in the router.
    if (!use_dor && precomputed_routes.empty()) {
        precomputed_routes = std::vector<std::vector<Route>>(devices_count, std::vector<Route>(devices_count));
    }

    std::set<int> touched;
    for (const auto& e : ocs_edges) {
        int u = e.first;
        int v = e.second;
        assert(u >= 0 && u < devices_count && v >= 0 && v < devices_count);
        // Under sparse connectivity the OCS pair may have no link yet; create
        // it so the override route can traverse it (no-op if already linked).
        topology->connect_ocs_edge(u, v, link_bw, link_lt);
        touched.insert(u);
        touched.insert(v);
    }

    for (const auto& path : routes) {
        if (path.empty()) {
            continue;
        }
        int s = path.front();
        int t = path.back();
        Route route;
        for (int id : path) {
            route.push_back(topology->get_device(id));
        }
        if (use_dor) {
            router_->set_override(s, t, std::move(route));
        } else {
            precomputed_routes[s][t] = std::move(route);
        }
        touched.insert(s);
    }

    // Reconfigure only the touched devices -- no global drain. DOR mode fetches
    // routes on demand, so push an empty route row (links only). scoped=true:
    // only the links this wiring actually changed are retuned, and the
    // per-device topology_iteration is left alone (see Device::reconfigure).
    for (int d : touched) {
        // connect_ocs_edge already created/updated the OCS link at link_bw, so
        // reading the device's current links back reproduces the row the old
        // live `bandwidths[d]`/`latencies[d]` mirror used to hold.
        BandwidthRow bw_row;
        LatencyRow lt_row;
        const auto dev = topology->get_device(d);
        for (const auto& [id, link] : dev->get_links()) {
            if (id == d) {
                continue;
            }
            bw_row[id] = link->get_bandwidth();
            lt_row[id] = link->get_latency();
        }
        static const std::vector<Route> kNoRoutes;
        dev->reconfigure(bw_row, use_dor ? kNoRoutes : precomputed_routes[d], lt_row, Latency(0), /*scoped=*/true);
    }

    if (std::getenv("RFOLD_WIRING_LOG") != nullptr) {
        std::cerr << "[wiring] ocs_edges=" << ocs_edges.size() << " routes=" << routes.size()
                  << " overrides=" << (router_ != nullptr ? router_->override_count() : precomputed_routes.size())
                  << " touched=" << touched.size() << std::endl;
    }
}

namespace {
// True iff u and v are adjacent in the base torus: coordinates differ by
// +-1 mod D in exactly one dimension. Same id encoding as Router (dims[0]
// fastest-varying). Independent of DOR arc direction (--bidi).
bool is_torus_neighbor(const std::vector<int>& dims, DeviceId u, DeviceId v) noexcept {
    if (dims.empty() || u == v) {
        return false;
    }
    int cu = u;
    int cv = v;
    int diff_dims = 0;
    bool adjacent = false;
    for (const int d : dims) {
        const int xu = cu % d;
        const int xv = cv % d;
        cu /= d;
        cv /= d;
        if (xu == xv) {
            continue;
        }
        diff_dims++;
        if (diff_dims > 1) {
            return false;
        }
        adjacent = (xv == (xu + 1) % d) || (xv == (xu - 1 + d) % d);
        if (!adjacent) {
            return false;
        }
    }
    return diff_dims == 1 && adjacent;
}
}  // namespace

void TopologyManager::remove_job_wiring(const std::vector<std::pair<int, int>>& ocs_edges,
                                        const std::vector<std::vector<int>>& routes) noexcept {
    if (!use_dor) {
        // The dynamic-scheduling flow is DOR-only; restoring a BFS eager-table
        // row would need a route recompute this path does not attempt.
        static bool warned = false;
        if (!warned) {
            std::cerr << "[unwiring] BFS mode: job-wiring teardown unsupported; overrides persist" << std::endl;
            warned = true;
        }
        return;
    }

    for (const auto& path : routes) {
        if (path.empty()) {
            continue;
        }
        router_->erase_override(path.front(), path.back());
    }

    for (const auto& e : ocs_edges) {
        const int u = e.first;
        const int v = e.second;
        assert(u >= 0 && u < devices_count && v >= 0 && v < devices_count);
        if (is_torus_neighbor(npus_per_dim, u, v)) {
            // Adjacent pairs never got an OCS link (connect_ocs_edge skips
            // already-connected pairs); zeroing their cells would take down a
            // real torus link.
            std::cerr << "[unwiring] warning: plan lists torus-adjacent OCS edge (" << u << ", " << v << "); skipping"
                      << std::endl;
            continue;
        }
        const auto du = topology->get_device(u);
        const auto dv = topology->get_device(v);
        if (!du->connected(v) || !dv->connected(u)) {
            std::cerr << "[unwiring] warning: OCS edge (" << u << ", " << v << ") not connected; skipping" << std::endl;
            continue;
        }
        if (du->get_links().at(v)->is_busy() || dv->get_links().at(u)->is_busy() || du->pending_chunks_count(v) > 0 ||
            dv->pending_chunks_count(u) > 0 ||
            (fluid_ && (flow_engine_->active_flows(du->get_links().at(v).get()) > 0 ||
                        flow_engine_->active_flows(dv->get_links().at(u).get()) > 0))) {
            // Structural invariant broken: the owning job is drained, so its
            // OCS link cannot be carrying or queueing traffic. Leak the link
            // rather than erase it under an outstanding link-free event
            // (use-after-free).
            std::cerr << "[unwiring] CRITICAL: OCS link (" << u << ", " << v
                      << ") busy or has pending chunks at teardown; leaking it" << std::endl;
            continue;
        }
        du->disconnect(v);
        dv->disconnect(u);
    }

    if (std::getenv("RFOLD_WIRING_LOG") != nullptr) {
        std::cerr << "[unwiring] ocs_edges=" << ocs_edges.size() << " routes=" << routes.size()
                  << " overrides=" << (router_ != nullptr ? router_->override_count() : precomputed_routes.size())
                  << std::endl;
    }
}

const Route& TopologyManager::get_precomputed_route(DeviceId src, DeviceId dst) const noexcept {
    if (router_ != nullptr) {
        return *router_->lookup(src, dst);
    }
    return precomputed_routes[src][dst];
}
