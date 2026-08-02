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

FlowEngine::~FlowEngine() noexcept {
    for (auto* const arg : finish_arg_pool) {
        delete arg;
    }
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

    const auto& route = chunk->get_route();
    const auto cursor = chunk->get_cursor();

    // Degenerate route (src == dst): nothing to transmit, deliver right away.
    if (route.size() - cursor < 2) {
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
    for (auto i = cursor; i + 1 < route.size(); ++i) {
        auto link = route[i]->get_link(route[i + 1]->get_id());
        latency_sum += link->get_latency();
        flow.links.push_back(std::move(link));
    }
    flow.total_latency = static_cast<EventTime>(latency_sum);
    flow.chunk = std::move(chunk);

    for (const auto& link : flow.links) {
        link_flows[link.get()].push_back(id);
        link->fluid_flow_inc();
    }
    auto& inserted = flows.emplace(id, std::move(flow)).first->second;

    // The touched set is exactly the flow's own links, so collect straight
    // from them instead of building a throwaway raw-pointer copy.
    scratch_affected.clear();
    for (const auto& link : inserted.links) {
        collect_flows_on(link.get());
    }
    recompute_collected();
}

int FlowEngine::active_flows(const Link* const link) const noexcept {
    // The count lives on the Link (maintained at register/deregister), so
    // this is O(1) with no hashing -- it is read per link per affected flow
    // in compute_rate on every flow start/finish.
    return link->fluid_flow_count();
}

void FlowEngine::on_link_updated(Link* const link) noexcept {
    scratch_affected.clear();
    collect_flows_on(link);
    recompute_collected();
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
        const auto count = link->fluid_flow_count();
        assert(count > 0);
        const double share = bw_GBps_to_Bpns(link->get_bandwidth()) / static_cast<double>(count);
        if (rate < 0.0 || share < rate) {
            rate = share;
        }
    }
    return rate < 0.0 ? 0.0 : rate;
}

void FlowEngine::reschedule(Flow& flow, const EventTime now) noexcept {
    // Lazy finish: the invariant is that the outstanding event (if any)
    // fires AT or BEFORE the flow's true finish time -- an early fire
    // re-arms itself in transmission_finished. So a rate DROP (finish moved
    // later, the common case when a new flow joins a shared link) keeps the
    // outstanding event; only a rate RISE (finish moved earlier) schedules
    // a new event and orphans the old one via the epoch.
    if (flow.rate <= 0.0) {
        return;  // parked; the outstanding event (if any) fires, makes no
                 // progress, clears next_fire, and a later recompute re-arms
    }
    const double eta = flow.remaining / flow.rate;
    const auto target = now + std::max<EventTime>(1, static_cast<EventTime>(std::ceil(eta)));
    if (flow.next_fire != 0 && flow.next_fire <= target) {
        return;  // outstanding event already fires no later than needed
    }
    flow.epoch++;
    flow.next_fire = target;
    FinishEventArg* arg;
    if (finish_arg_pool.empty()) {
        arg = new FinishEventArg{this, flow.id, flow.epoch};
    } else {
        arg = finish_arg_pool.back();
        finish_arg_pool.pop_back();
        *arg = {this, flow.id, flow.epoch};
    }
    event_queue->schedule_event(target, transmission_finished, static_cast<void*>(arg));
}

void FlowEngine::collect_flows_on(const Link* const link) noexcept {
    const auto it = link_flows.find(link);
    if (it == link_flows.end()) {
        return;
    }
    scratch_affected.insert(scratch_affected.end(), it->second.begin(), it->second.end());
}

void FlowEngine::recompute_collected() noexcept {
    const auto now = event_queue->get_current_time();

    // Process the (unique) flows crossing any touched link. Counts change
    // only on flow add/remove, so one level suffices — no propagation.
    auto& affected = scratch_affected;
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
    auto* const fe_arg = static_cast<FinishEventArg*>(arg);
    auto* const self = fe_arg->engine;
    const auto flow_id = fe_arg->flow_id;
    const auto arg_epoch = fe_arg->epoch;
    // Fields copied out; return the arg to the freelist up front (a
    // reschedule later in this very call may pop and reuse it).
    self->finish_arg_pool.push_back(fe_arg);

    const auto it = self->flows.find(flow_id);
    if (it == self->flows.end() || it->second.epoch != arg_epoch) {
        return;  // stale event: the flow was rescheduled or already done
    }

    auto& flow = it->second;
    const auto now = self->event_queue->get_current_time();
    self->advance(flow, now);
    if (flow.remaining > 0.0) {
        // Early fire: the rate dropped (or the flow parked) after this event
        // was armed, so bytes are still draining. Re-arm at the current
        // estimate and keep the flow alive.
        flow.next_fire = 0;
        self->reschedule(flow, now);
        return;
    }

    // Deregister from every link; freed capacity may speed up neighbors. A
    // link whose registry empties notifies the drain machinery (the hook is
    // a no-op outside a reconfigure drain).
    for (const auto& link : flow.links) {
        auto& ids = self->link_flows.at(link.get());
        // Swap-and-pop: O(1) removal instead of an O(F) shift. The per-link
        // id order is irrelevant -- recompute_flows_on sorts the affected
        // set before processing, so results are unchanged.
        const auto pos = std::find(ids.begin(), ids.end(), flow.id);
        assert(pos != ids.end());
        *pos = ids.back();
        ids.pop_back();
        link->fluid_flow_dec();
        if (ids.empty()) {
            self->link_flows.erase(link.get());
            Link::increment_callback();
        }
    }

    auto chunk = std::move(flow.chunk);
    const auto latency = flow.total_latency;
    // The touched set is the flow's own links; keep them alive past the
    // erase below instead of building a raw-pointer copy.
    const auto links = std::move(flow.links);
    // Erase by key, not by `it`: increment_callback() above can reentrantly
    // complete a drain -> retune -> resume() -> begin_flow() -> flows.emplace(),
    // and a rehash invalidates `it` (only references survive rehash).
    self->flows.erase(flow_id);
    // Collect only now, after the deregister loop: increment_callback()
    // above can reentrantly run begin_flow, which shares scratch_affected.
    // From here on nothing reenters until recompute_collected returns.
    self->scratch_affected.clear();
    for (const auto& link : links) {
        self->collect_flows_on(link.get());
    }
    self->recompute_collected();

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
