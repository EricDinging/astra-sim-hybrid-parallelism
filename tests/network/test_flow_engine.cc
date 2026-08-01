// Unit tests for the fluid congestion model's FlowEngine: rate = min over
// path links of (bw / active_flow_count), finish = path latency + bytes
// drained through rate changes. Built on tiny hand-wired topologies.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "common/EventQueue.h"
#include "common/NetworkFunction.h"
#include "reconfigurable/Chunk.h"
#include "reconfigurable/Device.h"
#include "reconfigurable/FlowEngine.h"
#include "reconfigurable/Link.h"
#include "reconfigurable/Topology.h"

using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

namespace {

// Arrival-time recorder passed as the chunk callback argument.
struct ArrivalLog {
    EventQueue* eq;
    std::vector<EventTime> times;
};

void record_arrival(void* arg) {
    auto* log = static_cast<ArrivalLog*>(arg);
    log->times.push_back(log->eq->get_current_time());
}

class FlowEngineTest : public ::testing::Test {
  public:
    // 4 devices, no links yet; tests wire links they need via connect_ocs_edge
    // (which creates a bidirectional link pair at the given bw/lt).
    void SetUp() override {
        event_queue = std::make_shared<EventQueue>();
        Topology::set_event_queue(event_queue);
        topology = std::make_shared<Topology>(4, 4);
        engine = std::make_unique<FlowEngine>(event_queue.get());
        log.eq = event_queue.get();
    }

    // Build [src, ..., dst] route from device ids.
    Route make_route(std::initializer_list<int> ids) {
        Route r;
        for (int id : ids) {
            r.push_back(topology->get_device(id));
        }
        return r;
    }

    void start(std::initializer_list<int> ids, ChunkSize size) {
        auto chunk = std::make_unique<Chunk>(
            size, make_route(ids), record_arrival, static_cast<void*>(&log),
            /*topology_iteration=*/0);
        engine->start_flow(std::move(chunk));
    }

    void run_to_completion() {
        while (!event_queue->finished()) {
            event_queue->proceed();
        }
    }

    std::shared_ptr<EventQueue> event_queue;
    std::shared_ptr<Topology> topology;
    std::unique_ptr<FlowEngine> engine;
    ArrivalLog log;
};

}  // namespace

// One flow, one link: arrival = latency + size/bw (same formula as serial).
TEST_F(FlowEngineTest, SingleFlowMatchesFormula) {
    topology->connect_ocs_edge(0, 1, Bandwidth(1.0), Latency(500.0));
    const ChunkSize size = 1024;
    start({0, 1}, size);
    run_to_completion();
    ASSERT_EQ(log.times.size(), 1U);
    const double bpns = bw_GBps_to_Bpns(1.0);
    const auto expected =
        static_cast<EventTime>(static_cast<double>(size) / bpns) + 500;
    // ceil/min-1ns bookkeeping may add up to 1 ns.
    EXPECT_NEAR(static_cast<double>(log.times[0]),
                static_cast<double>(expected), 2.0);
}

// Two same-size flows sharing one link from t=0: each runs at bw/2, both
// finish at ~2*size/bw (fair sharing, not FIFO serialization).
TEST_F(FlowEngineTest, TwoFlowsShareLinkEvenly) {
    topology->connect_ocs_edge(0, 1, Bandwidth(1.0), Latency(0.0));
    const ChunkSize size = 1 << 20;
    start({0, 1}, size);
    start({0, 1}, size);
    run_to_completion();
    ASSERT_EQ(log.times.size(), 2U);
    const double bpns = bw_GBps_to_Bpns(1.0);
    const double expected = 2.0 * static_cast<double>(size) / bpns;
    EXPECT_NEAR(static_cast<double>(log.times[0]), expected, 4.0);
    EXPECT_NEAR(static_cast<double>(log.times[1]), expected, 4.0);
}

// A flow speeds back up when its competitor finishes (the spec's worked
// example): f1 (1000B) alone until t=400, then shares; f2 finishes last at
// full rate. With bw such that 1 B/ns: f1 ends ~1600, f2 ends ~2000.
TEST_F(FlowEngineTest, MidFlightSpeedChange) {
    // Pick bw so bw_GBps_to_Bpns(bw) == 1.0 exactly: bw = 1e9/2^30 GB/s.
    const Bandwidth bw = 1e9 / static_cast<double>(1ULL << 30);
    topology->connect_ocs_edge(0, 1, bw, Latency(0.0));
    start({0, 1}, 1000);
    // Inject the second flow at t=400 via a scheduled event.
    struct Injector {
        FlowEngineTest* self;
    } inj{this};
    event_queue->schedule_event(
        400,
        [](void* a) {
            auto* i = static_cast<Injector*>(a);
            i->self->start({0, 1}, 1000);
        },
        &inj);
    run_to_completion();
    ASSERT_EQ(log.times.size(), 2U);
    EXPECT_NEAR(static_cast<double>(log.times[0]), 1600.0, 4.0);  // f1
    EXPECT_NEAR(static_cast<double>(log.times[1]), 2000.0, 4.0);  // f2
}

// Multi-hop flow is pipelined: rate = min link share, latency added once per
// hop, and NO per-hop re-serialization (the serial model would pay
// size/bw twice here).
TEST_F(FlowEngineTest, MultiHopBottleneckNoStoreAndForward) {
    topology->connect_ocs_edge(0, 1, Bandwidth(2.0), Latency(100.0));
    topology->connect_ocs_edge(1, 2, Bandwidth(1.0), Latency(100.0));
    const ChunkSize size = 1 << 20;
    start({0, 1, 2}, size);
    run_to_completion();
    ASSERT_EQ(log.times.size(), 1U);
    const double bottleneck = bw_GBps_to_Bpns(1.0);
    const double expected = static_cast<double>(size) / bottleneck + 200.0;
    EXPECT_NEAR(static_cast<double>(log.times[0]), expected, 4.0);
    // Strictly cheaper than store-and-forward (which adds size/(2 GB/s) more).
    const double serial_extra =
        static_cast<double>(size) / bw_GBps_to_Bpns(2.0);
    EXPECT_LT(static_cast<double>(log.times[0]), expected + serial_extra);
}

// Three flows on one link each get bw/3.
TEST_F(FlowEngineTest, ThreeWaySharing) {
    topology->connect_ocs_edge(0, 1, Bandwidth(1.0), Latency(0.0));
    const ChunkSize size = 1 << 20;
    start({0, 1}, size);
    start({0, 1}, size);
    start({0, 1}, size);
    run_to_completion();
    ASSERT_EQ(log.times.size(), 3U);
    const double expected =
        3.0 * static_cast<double>(size) / bw_GBps_to_Bpns(1.0);
    for (const auto t : log.times) {
        EXPECT_NEAR(static_cast<double>(t), expected, 6.0);
    }
}

// A zero-bandwidth link parks the flow (no events, active on the link);
// raising the bandwidth and notifying the engine resumes it.
TEST_F(FlowEngineTest, ZeroBwParksAndResumes) {
    topology->connect_ocs_edge(0, 1, Bandwidth(0.0), Latency(0.0));
    start({0, 1}, 1024);
    auto link = topology->get_device(0)->get_link(1);
    EXPECT_EQ(engine->active_flows(link.get()), 1);
    EXPECT_TRUE(event_queue->finished());  // parked: nothing scheduled
    // Retune the link to 1 GB/s (reconfigure marks it busy; free it) and
    // notify the engine.
    link->reconfigure(Bandwidth(1.0), Latency(0.0), Latency(0.0));
    link->set_free();
    engine->on_link_updated(link.get());
    run_to_completion();
    ASSERT_EQ(log.times.size(), 1U);
    EXPECT_EQ(engine->active_flows(link.get()), 0);
}

// pause() defers new flows entirely (no link registration); resume() starts
// them.
TEST_F(FlowEngineTest, PauseDefersResumeStarts) {
    topology->connect_ocs_edge(0, 1, Bandwidth(1.0), Latency(0.0));
    engine->pause();
    start({0, 1}, 1024);
    auto link = topology->get_device(0)->get_link(1);
    EXPECT_EQ(engine->active_flows(link.get()), 0);
    EXPECT_TRUE(event_queue->finished());
    engine->resume();
    run_to_completion();
    ASSERT_EQ(log.times.size(), 1U);
}
