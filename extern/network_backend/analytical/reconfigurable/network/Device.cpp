/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "reconfigurable/Device.h"
#include "common/HelperFunction.h"
#include "reconfigurable/Chunk.h"
#include "reconfigurable/Link.h"
#include "reconfigurable/Router.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace NetworkAnalyticalReconfigurable;

std::function<void()> Device::increment_callback = []() {};
std::function<void(Link*)> Device::link_freed_hook = [](Link*) noexcept {};
std::function<int(const Link*)> Device::flow_count_probe = [](const Link*) noexcept { return 0; };

Device::Device(const DeviceId id) noexcept : device_id(id), topology_iteration(0) {
    assert(id >= 0);
}

DeviceId Device::get_id() const noexcept {
    assert(device_id >= 0);
    return device_id;
}

std::shared_ptr<Link> Device::get_link(const DeviceId id) const noexcept {
    assert(id >= 0);
    assert(connected(id));
    return links.at(id);
}

int Device::pending_chunks_count(const DeviceId id) const noexcept {
    assert(id >= 0);
    assert(connected(id));
    return static_cast<int>(pending_chunks.at(id).size());
}

void Device::link_become_free(DeviceId link_id) noexcept {
    // one lookup per map; hold the references for the rest of the call
    const auto link_it = links.find(link_id);
    assert(link_it != links.end());
    const auto& link = link_it->second;
    const auto pending_it = pending_chunks.find(link_id);
    assert(pending_it != pending_chunks.end());
    auto& pending = pending_it->second;

    // set link free
    link->set_free();
    link_freed_hook(link.get());
    // std::cout << "Device " << device_id << ": link to " << link_id << " is free at time " << Link::get_current_time()
    // << std::endl;

    // process pending chunks if one exist
    if (pending.empty() || pending.front()->get_topology_iteration() > topology_iteration) {
        // Each link must report drained exactly once. If fluid flows are still
        // registered on this link (link_freed_hook above may have un-parked some
        // of them), the FlowEngine's own link-empty notification is the single
        // report for this link, so skip ours to avoid double-counting.
        if (flow_count_probe(link.get()) == 0) {
            increment_callback();
        }
        return;
    }

    // printf("Pending chunk topology iteration: %d, current topology iteration: %d\n",
    //        pending.front()->get_topology_iteration(), topology_iteration);

    if (link->get_bandwidth() == Bandwidth(0)) {
        // A pending chunk must stay queued while the link has no bandwidth:
        // sending would compute an infinite serialization delay and cast it
        // to an integer event time (UB). Reachable when a reconfigure
        // schedules free events for a still-0-BW link (BFS mode, or a bad
        // schedule file in DOR mode).
        return;
    }

    std::unique_ptr<Chunk> chunk = std::move(pending.front());
    pending.pop_front();

    auto next_link_free_time = link->send(std::move(chunk));

    // std::cout << "Device " << device_id << ": link to " << link_id << " becomes free at time and scheduled another
    // chunk " << next_link_free_time << ", link pending chunk: " << pending.size() << std::endl;

    // schedule the next link free event (arg preallocated in the link)
    Link::schedule_event(next_link_free_time, link_become_free, link->free_callback_arg());
}

void Device::link_become_free(void* const arg) noexcept {
    assert(arg != nullptr);
    const auto* const callback_arg = static_cast<const LinkFreeCallbackArg*>(arg);
    assert(callback_arg->device != nullptr);
    assert(callback_arg->link_id >= 0);

    // invoke the link become free method on the device; the argument is owned
    // by the link (preallocated), so nothing to free here
    callback_arg->device->link_become_free(callback_arg->link_id);
}

void Device::send(std::unique_ptr<Chunk> chunk) noexcept {
    // assert the validity of the chunk
    assert(chunk != nullptr);

    // assert this node is the current source of the chunk
    assert(chunk->current_device()->get_id() == device_id);

    // assert the chunk hasn't arrived its final destination yet
    assert(!chunk->arrived_dest());

    // Print out the route
    // for (const auto& [id, route] : routes) {
    //     std::cout << "Route to device " << id << ": ";
    //     for (const auto& hop : route) {
    //         std::cout << hop->get_id() << " ";
    //     }
    //     std::cout << std::endl;
    // }
    if (chunk->get_topology_iteration() < topology_iteration) {
        // The chunk was routed on an older topology: recompute its route to
        // its true destination (route.back()). Refreshing toward the *next
        // hop* here truncated the route to one hop and delivered the chunk
        // there as if it had arrived (P0-1).
        const DeviceId dest_id = chunk->get_route().back()->get_id();
        // DOR mode: fetch the route on demand; BFS mode: use the per-row copy.
        if (router_ != nullptr) {
            chunk->update_route(router_->lookup(device_id, dest_id), topology_iteration);
        } else {
            chunk->update_route(std::make_shared<const Route>(routes[dest_id]), topology_iteration);
        }
    }

    // get next dest
    const auto next_dest = chunk->next_device();
    const auto next_dest_id = next_dest->get_id();

    // assert the next dest is connected to this node
    assert(connected(next_dest_id));

    // one lookup; hold the reference
    const auto link_it = links.find(next_dest_id);
    assert(link_it != links.end());
    const auto& link = link_it->second;

    if (link->is_busy() || link->get_bandwidth() == Bandwidth(0) ||
        chunk->get_topology_iteration() > topology_iteration) {
        // link is busy, add the chunk to pending chunks
        auto& pending = pending_chunks.find(next_dest_id)->second;
        pending.push_back(std::move(chunk));
        if (kVerboseLogging) {
            debug_print("Device " + std::to_string(device_id) + ": link to " + std::to_string(next_dest_id) +
                        " is busy or reconfiguring, adding chunk to pending queue. Pending queue size: " +
                        std::to_string(pending.size()));
        }
        return;
    }

    // send the chunk to the next dest
    // delegate this task to the link (link-free event arg preallocated in it)
    auto link_free_time = link->send(std::move(chunk));
    Link::schedule_event(link_free_time, link_become_free, link->free_callback_arg());
}

void Device::connect(const DeviceId id, const Bandwidth bandwidth, const Latency latency) noexcept {
    assert(id >= 0);
    assert(bandwidth >= 0);
    assert(latency >= 0);

    // assert there's no existing connection
    assert(!connected(id));

    // create link, binding its preallocated link-free callback arg to us
    auto link = std::make_shared<Link>(bandwidth, latency);
    link->set_free_callback_arg(this, id);
    links[id] = std::move(link);
    pending_chunks[id] = std::list<std::unique_ptr<Chunk>>();
}

void Device::reconfigure(const BandwidthRow& bandwidth,
                         const std::vector<Route>& routes,
                         const LatencyRow& latency,
                         Latency reconfig_time,
                         bool scoped) noexcept {
    // bandwidth/latency are sparse per-device rows (absent == 0); links are
    // sparse too (~6 torus neighbors + OCS edges + self-loop), so we look up
    // each link id in the row rather than indexing a full-width vector.
    if (!scoped) {
        // Per-job (scoped) wiring must not bump the iteration: chunks are
        // stamped with TopologyManager's GLOBAL iteration, so a per-device
        // bump here desyncs the counters and every chunk later transiting
        // this device is treated as stale (P0-1).
        topology_iteration++;
    }

    for (const auto& [id, link] : links) {
        assert(id >= 0);

        if (id == device_id) {
            continue;
        }

        assert(connected(id));

        const auto bw_it = bandwidth.find(id);
        const Bandwidth bw = (bw_it == bandwidth.end()) ? Bandwidth(0) : bw_it->second;
        const auto lt_it = latency.find(id);
        const Latency lt = (lt_it == latency.end()) ? Latency(0) : lt_it->second;
        assert(bw >= 0 && lt >= 0);

        if (scoped) {
            if (bw == link->get_bandwidth() && lt == link->get_latency()) {
                // Link untouched by this job's wiring: leave its busy state
                // and pending queue alone. Scheduling the unconditional +1ns
                // free event here force-freed busy links (P0-2).
                continue;
            }
            if (link->is_busy() || flow_count_probe(link.get()) > 0) {
                // Retuning a link mid-transmission would invalidate its
                // in-flight completion events (P4-11: Link::reconfigure
                // asserts !busy, aborting the run). Keep the old values and
                // warn; only reachable by re-wiring a previously leaked busy
                // OCS link.
                std::cerr << "[wiring] warning: device " << device_id << " link to " << id
                          << " busy at scoped reconfigure; keeping old bw/lt" << std::endl;
                continue;
            }
        }

        // update the route (BFS mode only; DOR mode passes an empty routes
        // vector and fetches on demand via router_).
        if (!routes.empty()) {
            this->routes[id] = routes[id];
        }
        // reconfigure the link
        if (kVerboseLogging) {
            debug_print("Device " + std::to_string(device_id) + ": Reconfiguring link to " + std::to_string(id) +
                        ", pending chunk size: " + std::to_string(pending_chunks[id].size()) +
                        ", new bandwidth: " + std::to_string(bw));
        }
        auto free_time = link->reconfigure(bw, lt, reconfig_time);
        // schedule the link free event (arg preallocated in the link)
        Link::schedule_event(free_time, link_become_free, link->free_callback_arg());
    }

    // std::vector<std::unique_ptr<Chunk>> pending_chunks_copy;
    // // move pending chunks to a temporary vector
    // for (auto& [dest_id, queue] : pending_chunks) {
    //     while (!queue.empty()) {
    //         std::unique_ptr<Chunk> chunk = std::move(queue.front());
    //         queue.pop_front();
    //         if(chunk->get_topology_iteration() > topology_iteration) {
    //             queue.push_back(std::move(chunk));
    //         }
    //         else {
    //             pending_chunks_copy.push_back(std::move(chunk));
    //         }
    //     }
    // }

    // // re-assign routes for the pending chunks
    // for (auto& chunk : pending_chunks_copy) {
    //     assert(chunk != nullptr);
    //     assert(chunk->current_device()->get_id() == device_id);

    //     // re-assign the route
    //     chunk->update_route(routes[chunk->next_device()->get_id()], 0);

    //     // re-send the chunk
    //     send(std::move(chunk));
    // }
}

void Device::disconnect(const DeviceId id) noexcept {
    assert(id >= 0);

    // assert there's an existing connection
    assert(connected(id));
    assert(pending_chunks[id].empty());

    // remove the link and its pending queue
    links.erase(id);
    pending_chunks.erase(id);
}

bool Device::connected(const DeviceId dest) const noexcept {
    assert(dest >= 0);

    // check whether the connection exists
    return links.find(dest) != links.end();
}
