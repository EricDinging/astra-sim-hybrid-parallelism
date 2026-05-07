#include "reconfigurable/TopologyManager.h"
#include "common/HelperFunction.h"
#include <algorithm>
#include <cassert>
#include <iostream>

using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

TopologyManager::TopologyManager(
    int npus_count,
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
    debug_print("BW schedules size: " +
                std::to_string(this->bw_schedules.size()));
    debug_print("LT schedules size: " +
                std::to_string(this->latency_schedules.size()));

    assert(npus_count > 0);
    assert(devices_count > 0);
    assert(devices_count >= npus_count);

    reconfiguring = false;

    topology = std::make_shared<Topology>(npus_count, devices_count);

    Link::increment_callback = [this]() noexcept {
        this->increment_callback();
    };

    Device::increment_callback = [this]() noexcept {
        this->increment_callback();
    };

    bandwidths.resize(devices_count,
                      std::vector<Bandwidth>(devices_count, Bandwidth(0)));
    latencies.resize(devices_count,
                     std::vector<Latency>(devices_count, Latency(0)));

    topology_iteration = 0;
    inflight_coll = 0;
    reconfig_time = Latency(0);

    if (!npus_per_dim.empty()) {
        set_topology_dims(npus_per_dim, is_torus, false);
    }
}

std::shared_ptr<Device> TopologyManager::get_device(const DeviceId deviceId) noexcept {
    // Validate the device ID
    assert(deviceId >= 0 && deviceId < devices_count);
    return topology->get_device(deviceId);
}

void TopologyManager::drain_network() noexcept {
    // Drain the network by iterating through all devices and their links
    Link::num_drained_links = 0;
    for (int i = 0; i < devices_count; ++i) {
        auto device = topology->get_device(i);
        device->draining = true;
        for (int j = 0; j < devices_count; ++j) {
            if (i != j) {
                auto link = device->get_link(j);
                if (!link->is_busy()) {
                    increment_callback();
                }
                // TODO what if the link is busy, or the link does not exist
            }
        }
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

    if (Link::num_drained_links < devices_count * (devices_count - 1)) {
        // TODO: what if not all devices are connected to each other?
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

        device->reconfigure(bandwidths[i], precomputed_routes[i], latencies[i], reconfig_time);
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
        precomputeRoutes_DOR();
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
        debug_print("Topology ID " + std::to_string(topo_id) +
                    " not found in BW/LT schedules.");
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

void TopologyManager::set_topology_dims(const std::vector<int>& npus_per_dim, bool is_torus,
                                        bool bidi) noexcept {
    assert(!npus_per_dim.empty());

    // Validate that the product of dims matches the number of NPUs
    int product = 1;
    for (int d : npus_per_dim) {
        assert(d > 0);
        product *= d;
    }
    assert(product == npus_count);

    this->npus_per_dim = npus_per_dim;
    this->dims_count   = static_cast<int>(npus_per_dim.size());
    this->is_torus     = is_torus;
    this->bidi         = bidi;
    this->use_dor      = true;  // enable DOR routing
}

void TopologyManager::precomputeRoutes_DOR() noexcept {
    assert(dims_count > 0 && "call set_topology_dims() before precomputeRoutes_DOR()");

    // -----------------------------------------------------------------------
    // Helper: translate a flat device ID to a per-dimension coordinate vector.
    //
    // The linearization convention is X-fastest (dim 0 is innermost):
    //   device_id = coord[0] + coord[1]*N0 + coord[2]*N0*N1 + ...
    //
    // To decode:
    //   coord[0] = device_id % N0
    //   coord[1] = (device_id / N0) % N1
    //   coord[2] = (device_id / (N0*N1)) % N2
    //   ...
    // -----------------------------------------------------------------------
    auto id_to_coord = [&](int id) -> std::vector<int> {
        std::vector<int> coord(dims_count);
        int remaining = id;
        for (int d = 0; d < dims_count; ++d) {
            coord[d] = remaining % npus_per_dim[d];
            remaining /= npus_per_dim[d];
        }
        return coord;
    };

    // Helper: convert a coordinate vector back to a flat device ID.
    auto coord_to_id = [&](const std::vector<int>& coord) -> int {
        int id = 0;
        int stride = 1;
        for (int d = 0; d < dims_count; ++d) {
            id += coord[d] * stride;
            stride *= npus_per_dim[d];
        }
        return id;
    };

    // Initialise the precomputed_routes table (same size as before).
    precomputed_routes = std::vector<std::vector<Route>>(
        devices_count, std::vector<Route>(devices_count));

    for (int s = 0; s < devices_count; ++s) {
        for (int t = 0; t < devices_count; ++t) {
            if (s == t) {
                precomputed_routes[s][t] = {topology->get_device(s)};
                continue;
            }

            Route path;
            std::vector<int> cur_coord = id_to_coord(s);
            // Always start the path at the source device.
            path.push_back(topology->get_device(coord_to_id(cur_coord)));

            const std::vector<int> dst_coord = id_to_coord(t);

            // Traverse dimensions in order: dim 0 (X) first, then Y, then Z…
            for (int d = 0; d < dims_count; ++d) {
                int Nd    = npus_per_dim[d];
                int delta = dst_coord[d] - cur_coord[d];  // signed displacement

                if (delta == 0) {
                    continue;  // already aligned on this dimension
                }

                // In a torus, the direction depends on flag bidi:
                //   false (default): always route in the id-increasing (+1) direction.
                //   true:            pick the shorter arc (forward or backward).
                int step = 1;  // direction of movement (+1 or -1)
                int hops;      // number of hops along this dimension

                if (is_torus) {
                    // Forward hops: number of +1 steps to reach dst from src.
                    int forward_hops  = (delta + Nd) % Nd;   // always >= 0
                    int backward_hops = Nd - forward_hops;   // always >= 0
                    if (bidi) {
                        // Bidirectional: pick the shorter arc.
                        if (forward_hops <= backward_hops) {
                            hops = forward_hops;
                            step = +1;
                        } else {
                            hops = backward_hops;
                            step = -1;
                        }
                    } else {
                        // Default: always route in the id-increasing (+1) direction.
                        hops = forward_hops;
                        step = +1;
                    }
                } else {
                    // Mesh: only one direction is possible.
                    hops = std::abs(delta);
                    step = (delta > 0) ? +1 : -1;
                }

                // Walk 'hops' steps along dimension d.
                for (int h = 0; h < hops; ++h) {
                    cur_coord[d] = (cur_coord[d] + step + Nd) % Nd;
                    path.push_back(topology->get_device(coord_to_id(cur_coord)));
                }
            }

            precomputed_routes[s][t] = std::move(path);
        }
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
        chunk->update_route(route(src, chunk->next_device()->get_id()), topology_iteration);
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
