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
                                 std::map<int, std::vector<std::vector<Bandwidth>>> bw_schedules,
                                 std::map<int, std::vector<std::vector<Latency>>> latency_schedules,
                                 std::vector<int> npus_per_dim,
                                 bool is_torus) noexcept {
    this->npus_count = npus_count;
    this->devices_count = devices_count;
    this->event_queue = event_queue;
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

    bandwidths.resize(devices_count, std::vector<Bandwidth>(devices_count, Bandwidth(0)));
    latencies.resize(devices_count, std::vector<Latency>(devices_count, Latency(0)));

    topology_iteration = 0;
    inflight_coll = 0;
    reconfig_time = Latency(0);

    if (!npus_per_dim.empty()) {
        // Baseline DOR is unidirectional (+1 arc per axis). Mapping-aware
        // placement policies (folding/rfold) install their own per-job routes
        // via apply_job_wiring(), which override this table for their ring
        // edges; everything else (firstfit, scatter baselines) routes here.
        set_topology_dims(npus_per_dim, is_torus, false);
    } else {
        // BFS mode (no geometry): fall back to all-pairs connectivity. Only
        // used for small non-torus / test topologies.
        topology->connect_all_pairs();
    }
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
        device->draining = true;
        for (const auto& [id, link] : device->get_links()) {
            if (id != i && !link->is_busy()) {
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
    debug_print("Drained Network, reconfiguring to TOPO ITERATION #" + std::to_string(topology_iteration));
    for (auto row : bandwidths) {
        std::string msg;
        for (auto bw : row) {
            msg += std::to_string(static_cast<int>(bw)) + " ";
        }
        debug_print(msg);
    }

    for (int i = 0; i < devices_count; ++i) {
        auto device = topology->get_device(i);
        // std::vector<Route> routes;
        // Create a route for each device
        std::vector<Bandwidth> bw_device = bandwidths[i];

        debug_print("BW Vector for device " + std::to_string(i) + ":");
        std::string msg;
        for (auto bw : bw_device) {
            msg += std::to_string(static_cast<int>(bw)) + " ";
        }
        debug_print(msg);

        // DOR mode fetches routes on demand (router_); pass an empty route row
        // so the device reconfigures links only. BFS mode pushes the full row.
        device->reconfigure(bandwidths[i], use_dor ? std::vector<Route>{} : precomputed_routes[i], latencies[i],
                            reconfig_time);
    }
}

bool TopologyManager::reconfigure(std::vector<std::vector<Bandwidth>> bandwidths,
                                  std::vector<std::vector<Latency>> latencies,
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
    for (auto row : bandwidths) {
        std::string msg;
        for (auto bw : row) {
            msg += std::to_string(static_cast<int>(bw)) + " ";
        }
        debug_print(msg);
    }

    assert(bandwidths.size() == devices_count);
    assert(latencies.size() == devices_count);
    assert(!reconfiguring);

    for (const auto& row : bandwidths) {
        assert(row.size() == devices_count);
    }
    for (const auto& row : latencies) {
        assert(row.size() == devices_count);
    }

    // Update the bandwidth and latency matrices
    this->bandwidths = bandwidths;
    this->latencies = latencies;
    this->reconfig_time = reconfig_time;

    if (use_dor && dims_count > 0) {
        // DOR is computed on demand; a full reconfigure == rebuild-to-pure-DOR,
        // i.e. drop overrides + cached routes.
        if (router_ != nullptr) {
            router_->clear();
        }
    } else {
        precomputeRoutes();
    }

    reconfiguring = true;
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

void TopologyManager::precomputeRoutes() noexcept {
    // TODO: add other routing algorithms
    // Adjacency list
    std::vector<std::vector<int>> adj(devices_count);
    for (int i = 0; i < devices_count; ++i) {
        for (int j = 0; j < devices_count; ++j) {
            if (i != j && bandwidths[i][j] > 0) {
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
                precomputed_routes[s][t] = move(path);
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
        // DOR mode: fetch on demand; BFS mode: index the eager table.
        const Route& r = (router_ != nullptr) ? router_->lookup(src, dest) : precomputed_routes[src][dest];
        chunk->update_route(r, topology_iteration);
    }

    // printf("TM: Sending chunk from %d to %d, in topo iter %d, route: ", chunk->current_device()->get_id(),
    // chunk->next_device()->get_id(), chunk->get_topology_iteration()); for(auto device : chunk->route){
    //     printf("%d ", device->get_id());
    // }
    // printf("\n");

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
        bandwidths[u][v] = bandwidths[v][u] = link_bw;
        latencies[u][v] = latencies[v][u] = link_lt;
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
    // routes on demand, so push an empty route row (links only).
    for (int d : touched) {
        topology->get_device(d)->reconfigure(bandwidths[d], use_dor ? std::vector<Route>{} : precomputed_routes[d],
                                             latencies[d], Latency(0));
    }

    if (std::getenv("RFOLD_WIRING_LOG") != nullptr) {
        std::cerr << "[wiring] ocs_edges=" << ocs_edges.size() << " routes=" << routes.size()
                  << " overrides=" << (router_ != nullptr ? router_->override_count() : precomputed_routes.size())
                  << " touched=" << touched.size() << std::endl;
    }
}

const Route& TopologyManager::get_precomputed_route(DeviceId src, DeviceId dst) const noexcept {
    if (router_ != nullptr) {
        return router_->lookup(src, dst);
    }
    return precomputed_routes[src][dst];
}

Bandwidth TopologyManager::get_cell_bandwidth(DeviceId u, DeviceId v) const noexcept {
    return bandwidths[u][v];
}
