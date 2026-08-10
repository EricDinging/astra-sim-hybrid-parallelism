/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "reconfigurable/Device.h"
#include "common/HelperFunction.h"
#include "reconfigurable/Chunk.h"
#include "reconfigurable/Link.h"
#include "reconfigurable/Router.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

using namespace NetworkAnalyticalReconfigurable;

std::function<void()> Device::increment_callback = []() {};
std::function<void(Link*)> Device::link_freed_hook = [](Link*) noexcept {};

// Starts at 1 so a fresh Route (links_epoch = 0) is always stale.
std::uint64_t Device::wiring_epoch = 1;

Device::Device(const DeviceId id) noexcept : device_id(id), topology_iteration(0) {
    assert(id >= 0);
}

DeviceId Device::get_id() const noexcept {
    assert(device_id >= 0);
    return device_id;
}

Device::PortVec::iterator Device::port_pos(const DeviceId id) noexcept {
    return std::lower_bound(
        ports_.begin(), ports_.end(), id,
        [](const std::pair<DeviceId, Port>& p, const DeviceId key) noexcept { return p.first < key; });
}

Device::PortVec::const_iterator Device::port_pos(const DeviceId id) const noexcept {
    return std::lower_bound(
        ports_.begin(), ports_.end(), id,
        [](const std::pair<DeviceId, Port>& p, const DeviceId key) noexcept { return p.first < key; });
}

Device::Port* Device::find_port(const DeviceId id) noexcept {
    const auto it = port_pos(id);
    return (it != ports_.end() && it->first == id) ? &it->second : nullptr;
}

const Device::Port* Device::find_port(const DeviceId id) const noexcept {
    const auto it = port_pos(id);
    return (it != ports_.end() && it->first == id) ? &it->second : nullptr;
}

std::shared_ptr<Link> Device::get_link(const DeviceId id) const noexcept {
    assert(id >= 0);
    const auto* const port = find_port(id);
    assert(port != nullptr);
    return port->link;
}

int Device::pending_chunks_count(const DeviceId id) const noexcept {
    assert(id >= 0);
    const auto* const port = find_port(id);
    assert(port != nullptr);
    return static_cast<int>(port->pending.size());
}

void Device::link_become_free(DeviceId link_id) noexcept {
    // one lookup; hold the references for the rest of the call
    auto* const port = find_port(link_id);
    assert(port != nullptr);
    const auto& link = port->link;
    auto& pending = port->pending;

    // Lazy firing: hot-path bookings may extend the link's occupancy past
    // this event's time (drain-report events are armed at the occupancy seen
    // when the drain started). Re-arm at the current end instead of freeing
    // early.
    const auto now = Link::current_time();
    if (!link->is_flag_busy() && now < link->next_free_time()) {
        Link::schedule_event(link->next_free_time(), link_become_free, link->free_callback_arg());
        return;
    }

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
        // report for this link, so skip ours to avoid double-counting. The
        // count lives on the Link itself and is 0 whenever fluid mode is off.
        if (link->fluid_flow_count() == 0) {
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
    link->set_has_pending(!pending.empty());

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
        // DOR mode: fetch the route on demand; BFS mode: share the stored
        // row (refcount bump, no deep copy; at() instead of operator[] so a
        // missing row fails loudly instead of fabricating an empty route).
        if (router_ != nullptr) {
            chunk->update_route(router_->lookup(device_id, dest_id), topology_iteration);
        } else {
            chunk->update_route(routes.at(dest_id), topology_iteration);
        }
    }

    // Per-route link cache: skip the port-table lower_bound AND the port
    // entry itself when this (route, hop) was already resolved under the
    // current wiring epoch. The epoch is bumped by every connect/disconnect/
    // reconfigure, so a cached pointer can never be read across a ports_
    // mutation; the cached link is exactly the one find_port(next_device
    // id)->link would return, so behavior is identical to the lookup. The
    // port's queue state is mirrored on the Link (has_pending), so the hot
    // path below reads only the Link's cache line. (Route is shared across
    // chunks; single-threaded sim, so the mutable writes are safe.)
    const auto& route = chunk->get_route();
    const auto cursor = chunk->get_cursor();
    if (route.links_epoch != wiring_epoch) {
        route.hop_links.assign(route.size(), nullptr);
        route.links_epoch = wiring_epoch;
    }
    Link* link = route.hop_links[cursor];
    if (link == nullptr) {
        // the assert below already proves the next dest is connected -- a
        // separate assert(connected(...)) would be a second search per hop,
        // and asserts are live in release here
        const auto next_dest_id = chunk->next_device()->get_id();
        const auto it = port_pos(next_dest_id);
        assert(it != ports_.end() && it->first == next_dest_id);
        link = it->second.link.get();
        route.hop_links[cursor] = link;
    }

    // Cold path: reconfiguration downtime or a cold transmission in flight
    // (flag), a dead link, a chunk stamped for a future topology, or a
    // non-empty pending queue (FIFO order with already-queued chunks must
    // hold; read via the Link's has_pending mirror). These keep the
    // event-driven machinery: the pending queue and link-free events carry
    // the reconfigure/drain semantics.
    // NOTE: arithmetic occupancy (next_free_time) deliberately does NOT
    // divert to the cold path -- hot sends append to the arithmetic booking
    // themselves, in FIFO order, with the same start times the event chain
    // produced.
    if (link->is_flag_busy() || link->get_bandwidth() == Bandwidth(0) ||
        chunk->get_topology_iteration() > topology_iteration || link->has_pending()) {
        const auto next_dest_id = chunk->next_device()->get_id();
        auto* const port = find_port(next_dest_id);
        assert(port != nullptr && port->link.get() == link);
        auto& pending = port->pending;
        pending.push_back(std::move(chunk));
        link->set_has_pending(true);
        if (kVerboseLogging) {
            debug_print("Device " + std::to_string(device_id) + ": link to " + std::to_string(next_dest_id) +
                        " is busy or reconfiguring, adding chunk to pending queue. Pending queue size: " +
                        std::to_string(pending.size()));
        }
        return;
    }

    // Hot path (busy-until): one arrival event, no link-free event.
    link->book_transmission(std::move(chunk));
}

void Device::connect(const DeviceId id, const Bandwidth bandwidth, const Latency latency) noexcept {
    assert(id >= 0);
    assert(bandwidth >= 0);
    assert(latency >= 0);

    // assert there's no existing connection
    assert(!connected(id));

    // create link, binding its preallocated link-free callback arg to us.
    // The arg points at the Device and the id -- not into ports_ -- so vector
    // reallocation on later inserts is safe.
    auto link = std::make_shared<Link>(bandwidth, latency);
    link->set_free_callback_arg(this, id);
    ports_.insert(port_pos(id), {id, Port{std::move(link), {}}});

    // ports_ changed (indices shifted, a new link exists): invalidate every
    // Route's cached port indices.
    ++wiring_epoch;
}

void Device::reconfigure(const BandwidthRow& bandwidth,
                         const std::vector<Route>& routes,
                         const LatencyRow& latency,
                         Latency reconfig_time,
                         bool scoped) noexcept {
    // Link retunes don't move ports_, but bump the epoch anyway (cold path,
    // and it keeps the invariant simple: any wiring-affecting operation
    // invalidates every Route's cached port indices).
    ++wiring_epoch;

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

    // ports_ is sorted ascending by DeviceId: identical iteration (and thus
    // same-tick event insertion) order as the std::map it replaced.
    for (auto& [id, port] : ports_) {
        assert(id >= 0);

        if (id == device_id) {
            continue;
        }

        const auto& link = port.link;

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
            if (link->is_busy() || link->fluid_flow_count() > 0) {
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
            this->routes[id] = std::make_shared<const Route>(routes[id]);
        }
        // reconfigure the link
        if (kVerboseLogging) {
            debug_print("Device " + std::to_string(device_id) + ": Reconfiguring link to " + std::to_string(id) +
                        ", pending chunk size: " + std::to_string(port.pending.size()) +
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
    const auto it = port_pos(id);
    assert(it != ports_.end() && it->first == id);
    assert(it->second.pending.empty());

    // remove the port (link + pending queue)
    ports_.erase(it);

    // ports_ changed (indices shifted, a link died): invalidate every Route's
    // cached port indices.
    ++wiring_epoch;
}

bool Device::connected(const DeviceId dest) const noexcept {
    assert(dest >= 0);

    // check whether the connection exists
    return find_port(dest) != nullptr;
}
