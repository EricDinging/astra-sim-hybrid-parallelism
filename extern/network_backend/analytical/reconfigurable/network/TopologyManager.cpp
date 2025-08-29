
#include "reconfigurable/TopologyManager.h"
#include <cassert>
#include <algorithm>
#include <iostream>

using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

TopologyManager::TopologyManager(int npus_count, int devices_count, EventQueue* event_queue) noexcept {
    // Initialize the number of NPUs
    this->npus_count = npus_count;
    this->devices_count = devices_count;
    this->event_queue = event_queue;

    // Validate the counts
    assert(npus_count > 0);
    assert(devices_count > 0);
    assert(devices_count >= npus_count);

    reconfiguring = false;

    // Initialize the topology
    topology = std::make_shared<Topology>(npus_count, devices_count);

    Link::increment_callback = [this]() noexcept {
        // Increment the topology iteration
        this->increment_callback();
    };

    Device::increment_callback = [this]() noexcept {
        // Increment the topology iteration
        this->increment_callback();
    };

    // Initialize bandwidth and latency matrices
    bandwidths.resize(devices_count, std::vector<Bandwidth>(devices_count, Bandwidth(0)));
    latencies.resize(devices_count, std::vector<Latency>(devices_count, Latency(0)));

    topology_iteration = 0;
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
        for(int j = 0; j < devices_count; ++j) {
            if (i != j) {
                auto link = device->get_link(j);
                if(!link->is_busy()) increment_callback();
            }
        }
    }
}

bool TopologyManager::is_reconfiguring() const noexcept {
    return reconfiguring;
}

void TopologyManager::increment_callback() noexcept {
    if(!reconfiguring){
        Link::num_drained_links = 0;
        return;
    }

    // Increment the topology iteration
    Link::num_drained_links++;
    // printf("Link drained: %d/%d at %d\n", Link::num_drained_links, devices_count * (devices_count - 1), Link::get_current_time());

    if(Link::num_drained_links < devices_count * (devices_count - 1)) {
        return;
    }

    Link::num_drained_links = 0;
    reconfiguring = false;

    // All links have been drained, increment the topology iteration
    std::cout << "Drained Network, reconfiguring to TOPO #" << topology_iteration << std::endl;

    for (int i = 0; i < devices_count; ++i) {
        auto device = topology->get_device(i);
        std::vector<Route> routes;
        // Create a route for each device
        for (int j = 0; j < devices_count; ++j) {
            routes.push_back(route(i, j));
        }
        device->reconfigure(this->bandwidths[i], routes, this->latencies[i], reconfig_time);
    }
}

void TopologyManager::reconfigure(std::vector<std::vector<Bandwidth>> bandwidths,
                               std::vector<std::vector<Latency>> latencies, Latency reconfig_time) noexcept {
    // Validate the new matrices

    while (is_reconfiguring() && !event_queue->finished()) {
        event_queue->proceed();
    }

    printf("Devuces count: %d, NPUs count: %d\n", devices_count, npus_count);
    printf("bandwidths size: %zu, latencies size: %zu\n", bandwidths.size(), latencies.size());

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
    this->bandwidths = std::move(bandwidths);
    this->latencies = std::move(latencies);
    this->reconfig_time = reconfig_time;

    printf("Reconfiguring topology with %d devices and %d NPUs.\n", devices_count, npus_count);

    reconfiguring = true;
    topology_iteration++;
    drain_network();
}

void TopologyManager::send(std::unique_ptr<Chunk> chunk) noexcept {
    assert(chunk != nullptr);
    assert(chunk->current_device() != nullptr);

    // chunk->update_route(route(chunk->current_device()->get_id(), chunk->next_device()->get_id()), topology_iteration);

    // Get the source device ID
    DeviceId src = chunk->current_device()->get_id();
    assert(src >= 0 && src < devices_count);

    if(chunk->get_topology_iteration() == -1){
        chunk->update_route(route(src, chunk->next_device()->get_id()), topology_iteration);
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