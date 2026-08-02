/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/EventQueue.h"
#include "common/Type.h"
#include "reconfigurable/Type.h"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace NetworkAnalytical;

namespace NetworkAnalyticalReconfigurable {

/**
 * FlowEngine implements the fluid congestion model (--congestion-model=fluid):
 * every routed chunk becomes a flow that occupies all links of its route
 * simultaneously at rate min over links of (bw / active_flow_count). The
 * chunk callback fires at path latency + the time its bytes take to drain
 * through rate changes. Finish events are lazy: each flow keeps at most one
 * outstanding event, armed to fire at or before the true finish time; an
 * early fire (the rate dropped after arming) re-arms itself. Rate rises
 * schedule a new, earlier event and orphan the old one via the epoch guard
 * (the shared EventQueue has no cancellation). Design:
 * docs/superpowers/specs/2026-07-25-fluid-congestion-model-design.md.
 */
class FlowEngine {
  public:
    explicit FlowEngine(EventQueue* event_queue) noexcept;

    /// Start a flow for a routed chunk (route = [src, ..., dst]); while
    /// paused, the chunk is deferred until resume().
    void start_flow(std::unique_ptr<Chunk> chunk) noexcept;

    /// Number of flows currently occupying the link (fluid-mode equivalent
    /// of Link::is_busy for drain / scoped-wiring / teardown checks).
    [[nodiscard]] int active_flows(const Link* link) const noexcept;

    /// Re-evaluate flows on a link after its bandwidth or busy state changed
    /// (link retune, reconfig-window link_become_free).
    void on_link_updated(Link* link) noexcept;

    /// Defer new flows during a global-reconfigure drain...
    void pause() noexcept;

    /// ...and release them once the post-drain retune is done.
    void resume() noexcept;

  private:
    struct Flow {
        std::unique_ptr<Chunk> chunk;
        std::vector<std::shared_ptr<Link>> links;  // directed links, in route order
        EventTime total_latency;                   // ns, summed at start
        double remaining;                          // bytes left to transmit
        double rate;                               // B/ns; 0 = parked
        EventTime last_update;                     // when `remaining` was banked
        uint64_t epoch;                            // invalidates stale events
        uint64_t id;
        // Absolute fire time of the outstanding finish event; 0 = none
        // outstanding (parked flow).
        EventTime next_fire = 0;
    };

    /// Heap-allocated argument of the epoch-guarded transmission-finish event.
    struct FinishEventArg {
        FlowEngine* engine;
        uint64_t flow_id;
        uint64_t epoch;
    };

    /// Event callback: transmission (bytes) done; deregister + deliver.
    static void transmission_finished(void* arg) noexcept;

    /// Event callback: latency elapsed; invoke the chunk's arrival callback.
    static void deliver(void* chunk_ptr) noexcept;

    void begin_flow(std::unique_ptr<Chunk> chunk) noexcept;
    void recompute_flows_on(const std::vector<Link*>& links) noexcept;
    void advance(Flow& flow, EventTime now) noexcept;
    [[nodiscard]] double compute_rate(const Flow& flow) const noexcept;
    void reschedule(Flow& flow, EventTime now) noexcept;

    EventQueue* event_queue;
    std::unordered_map<uint64_t, Flow> flows;
    std::unordered_map<const Link*, std::vector<uint64_t>> link_flows;
    std::vector<std::unique_ptr<Chunk>> deferred;
    uint64_t next_flow_id = 0;
    bool paused = false;
};

}  // namespace NetworkAnalyticalReconfigurable
