/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "reconfigurable/FlowEngine.h"
#include "common/NetworkFunction.h"
#include "reconfigurable/Chunk.h"
#include "reconfigurable/Device.h"
#include "reconfigurable/Link.h"
#include <algorithm>
#include <cassert>
#include <cmath>

using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

FlowEngine::FlowEngine(EventQueue* const event_queue) noexcept : event_queue(event_queue) {
    assert(event_queue != nullptr);
}

void FlowEngine::start_flow(std::unique_ptr<Chunk> chunk) noexcept {
    assert(chunk != nullptr);
    if (paused) {
        deferred.push_back(std::move(chunk));
        return;
    }
    begin_flow(std::move(chunk));
}

void FlowEngine::begin_flow(std::unique_ptr<Chunk> chunk) noexcept {
    const auto now = event_queue->get_current_time();

    // Degenerate route (src == dst): nothing to transmit, deliver right away.
    if (chunk->route.size() < 2) {
        event_queue->schedule_event(now + 1, deliver, static_cast<void*>(chunk.release()));
        return;
    }

    const auto id = next_flow_id++;
    Flow flow;
    flow.id = id;
    flow.remaining = static_cast<double>(chunk->get_size());
    flow.rate = 0.0;
    flow.last_update = now;
    flow.epoch = 0;

    // Map the device route to the directed links it traverses and sum the
    // path latency (snapshotted at start, per the spec).
    double latency_sum = 0.0;
    auto it = chunk->route.begin();
    for (auto next = std::next(it); next != chunk->route.end(); ++it, ++next) {
        auto link = (*it)->get_link((*next)->get_id());
        latency_sum += link->get_latency();
        flow.links.push_back(std::move(link));
    }
    flow.total_latency = static_cast<EventTime>(latency_sum);
    flow.chunk = std::move(chunk);

    std::vector<Link*> touched;
    touched.reserve(flow.links.size());
    for (const auto& link : flow.links) {
        link_flows[link.get()].push_back(id);
        touched.push_back(link.get());
    }
    flows.emplace(id, std::move(flow));

    recompute_flows_on(touched);
}

int FlowEngine::active_flows(const Link* const link) const noexcept {
    const auto it = link_flows.find(link);
    return it == link_flows.end() ? 0 : static_cast<int>(it->second.size());
}

void FlowEngine::on_link_updated(Link* const link) noexcept {
    recompute_flows_on({link});
}

void FlowEngine::pause() noexcept {
    paused = true;
}

void FlowEngine::resume() noexcept {
    paused = false;
    // Swap first: callbacks fired by begin_flow may re-enter start_flow.
    std::vector<std::unique_ptr<Chunk>> pending;
    pending.swap(deferred);
    for (auto& chunk : pending) {
        begin_flow(std::move(chunk));
    }
}

void FlowEngine::advance(Flow& flow, const EventTime now) noexcept {
    if (flow.rate > 0.0 && now > flow.last_update) {
        flow.remaining -= flow.rate * static_cast<double>(now - flow.last_update);
        if (flow.remaining < 0.0) {
            flow.remaining = 0.0;
        }
    }
    flow.last_update = now;
}

double FlowEngine::compute_rate(const Flow& flow) const noexcept {
    double rate = -1.0;
    for (const auto& link : flow.links) {
        // A busy link is mid-reconfiguration downtime: no capacity until the
        // link-free event lands (on_link_updated re-rates the flow then).
        if (link->is_busy() || link->get_bandwidth() <= Bandwidth(0)) {
            return 0.0;
        }
        const auto count = active_flows(link.get());
        assert(count > 0);
        const double share = bw_GBps_to_Bpns(link->get_bandwidth()) / static_cast<double>(count);
        if (rate < 0.0 || share < rate) {
            rate = share;
        }
    }
    return rate < 0.0 ? 0.0 : rate;
}

void FlowEngine::reschedule(Flow& flow, const EventTime now) noexcept {
    // Any previously scheduled finish event is now stale.
    flow.epoch++;
    if (flow.rate <= 0.0) {
        return;  // parked; a later recompute will reschedule
    }
    const double eta = flow.remaining / flow.rate;
    const auto delta = std::max<EventTime>(1, static_cast<EventTime>(std::ceil(eta)));
    auto* const arg = new FinishEventArg{this, flow.id, flow.epoch};
    event_queue->schedule_event(now + delta, transmission_finished, static_cast<void*>(arg));
}

void FlowEngine::recompute_flows_on(const std::vector<Link*>& links) noexcept {
    const auto now = event_queue->get_current_time();

    // Collect the (unique) flows crossing any touched link. Counts change
    // only on flow add/remove, so one level suffices — no propagation.
    std::vector<uint64_t> affected;
    for (const auto* link : links) {
        const auto it = link_flows.find(link);
        if (it == link_flows.end()) {
            continue;
        }
        affected.insert(affected.end(), it->second.begin(), it->second.end());
    }
    std::sort(affected.begin(), affected.end());
    affected.erase(std::unique(affected.begin(), affected.end()), affected.end());

    for (const auto id : affected) {
        auto& flow = flows.at(id);
        advance(flow, now);
        const double new_rate = compute_rate(flow);
        if (new_rate != flow.rate) {
            flow.rate = new_rate;
            reschedule(flow, now);
        }
    }
}

void FlowEngine::transmission_finished(void* const arg) noexcept {
    assert(arg != nullptr);
    const auto guard = std::unique_ptr<FinishEventArg>(static_cast<FinishEventArg*>(arg));
    auto* const self = guard->engine;

    const auto it = self->flows.find(guard->flow_id);
    if (it == self->flows.end() || it->second.epoch != guard->epoch) {
        return;  // stale event: the flow was rescheduled or already done
    }

    auto& flow = it->second;
    const auto now = self->event_queue->get_current_time();
    self->advance(flow, now);
    // ceil() overshoots by <1 ns of drain, so remaining is ~0 here.
    assert(flow.remaining < flow.rate + 1.0);

    // Deregister from every link; freed capacity may speed up neighbors. A
    // link whose registry empties notifies the drain machinery (the hook is
    // a no-op outside a reconfigure drain).
    std::vector<Link*> touched;
    touched.reserve(flow.links.size());
    for (const auto& link : flow.links) {
        auto& ids = self->link_flows.at(link.get());
        ids.erase(std::remove(ids.begin(), ids.end(), flow.id), ids.end());
        if (ids.empty()) {
            self->link_flows.erase(link.get());
            Link::increment_callback();
        }
        touched.push_back(link.get());
    }

    auto chunk = std::move(flow.chunk);
    const auto latency = flow.total_latency;
    self->flows.erase(it);
    self->recompute_flows_on(touched);

    if (latency > 0) {
        self->event_queue->schedule_event(now + latency, deliver, static_cast<void*>(chunk.release()));
    } else {
        chunk->invoke_callback();
    }
}

void FlowEngine::deliver(void* const chunk_ptr) noexcept {
    assert(chunk_ptr != nullptr);
    const auto chunk = std::unique_ptr<Chunk>(static_cast<Chunk*>(chunk_ptr));
    chunk->invoke_callback();
}
