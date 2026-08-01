// Integration tests: TopologyManager in fluid mode (set_fluid_mode(true))
// delivers chunks with fluid timing, defers traffic across a global
// reconfigure drain, and leaves serial mode untouched.

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <vector>

#include "common/EventQueue.h"
#include "common/NetworkFunction.h"
#include "reconfigurable/Chunk.h"
#include "reconfigurable/Topology.h"
#include "reconfigurable/TopologyManager.h"

using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

namespace {

struct ArrivalLog {
    EventQueue* eq;
    std::vector<EventTime> times;
};

void record_arrival(void* arg) {
    auto* log = static_cast<ArrivalLog*>(arg);
    log->times.push_back(log->eq->get_current_time());
}

// 3-device BFS-mode (all-pairs) manager. Topo 0: every off-diagonal pair at
// `bw` GB/s, zero latency. Topo 1 (used by the drain test): same shape at
// double bandwidth so the post-reconfigure rate is distinguishable.
class TmFluidTest : public ::testing::Test {
  protected:
    static constexpr int N = 3;

    void SetUp() override {
        event_queue = std::make_shared<EventQueue>();
        Topology::set_event_queue(event_queue);

        std::map<int, std::vector<BandwidthRow>> bw;
        std::map<int, std::vector<LatencyRow>> lt;
        for (int topo = 0; topo < 2; ++topo) {
            const Bandwidth b = topo == 0 ? Bandwidth(1.0) : Bandwidth(2.0);
            std::vector<BandwidthRow> bw_mat(N);
            std::vector<LatencyRow> lt_mat(N);
            for (int i = 0; i < N; ++i) {
                for (int j = 0; j < N; ++j) {
                    if (i != j) {
                        bw_mat[i][j] = b;
                        lt_mat[i][j] = Latency(0.0);
                    }
                }
            }
            bw[topo] = std::move(bw_mat);
            lt[topo] = std::move(lt_mat);
        }
        tm = std::make_shared<TopologyManager>(N, N, event_queue.get(),
                                               std::move(bw), std::move(lt));
        log.eq = event_queue.get();
    }

    void send(int src, int dst, ChunkSize size) {
        auto chunk =
            std::make_unique<Chunk>(size, tm->route(src, dst), record_arrival,
                                    static_cast<void*>(&log));
        tm->send(std::move(chunk));
    }

    void run_to_completion() {
        while (!event_queue->finished()) {
            event_queue->proceed();
        }
    }

    std::shared_ptr<EventQueue> event_queue;
    std::shared_ptr<TopologyManager> tm;
    ArrivalLog log;
};

}  // namespace

// Two flows sharing the 0->1 link in fluid mode finish together at
// ~2*size/bw — the fluid signature (serial FIFO would stagger them).
TEST_F(TmFluidTest, FluidSharingThroughSend) {
    tm->set_fluid_mode(true);
    tm->reconfigure(0);
    run_to_completion();  // flush reconfigure bookkeeping events

    const ChunkSize size = 1 << 20;
    send(0, 1, size);
    send(0, 1, size);
    run_to_completion();

    ASSERT_EQ(log.times.size(), 2U);
    const double expected =
        2.0 * static_cast<double>(size) / bw_GBps_to_Bpns(1.0);
    EXPECT_NEAR(static_cast<double>(log.times[0]), expected, 4.0);
    EXPECT_NEAR(static_cast<double>(log.times[1]), expected, 4.0);
}

// Serial mode is untouched: the same two sends FIFO-serialize (first at
// ~size/bw, second at ~2*size/bw).
TEST_F(TmFluidTest, SerialModeStillSerializes) {
    tm->reconfigure(0);
    run_to_completion();

    const ChunkSize size = 1 << 20;
    send(0, 1, size);
    send(0, 1, size);
    run_to_completion();

    ASSERT_EQ(log.times.size(), 2U);
    const double one = static_cast<double>(size) / bw_GBps_to_Bpns(1.0);
    EXPECT_NEAR(static_cast<double>(log.times[0]), one, 4.0);
    EXPECT_NEAR(static_cast<double>(log.times[1]), 2.0 * one, 4.0);
}

// A global reconfigure waits for in-flight flows (drain) and defers new
// traffic; the deferred flow then runs on the new (2x) bandwidth.
TEST_F(TmFluidTest, ReconfigureDrainsAndDefers) {
    tm->set_fluid_mode(true);
    // Production always sets a reconfig latency (main.cc); a nonzero value
    // also keeps post-drain retune events strictly after the drain tick.
    tm->set_reconfig_latency(Latency(1));
    tm->reconfigure(0);
    run_to_completion();

    const ChunkSize size = 1 << 20;
    send(0, 1, size);  // in flight on topo 0
    ASSERT_TRUE(tm->reconfigure(1));
    EXPECT_TRUE(tm->is_reconfiguring());  // drain pending on the in-flight flow

    send(0, 1, size);  // arrives mid-drain: must be deferred, not started
    run_to_completion();

    ASSERT_EQ(log.times.size(), 2U);
    const double first = static_cast<double>(size) / bw_GBps_to_Bpns(1.0);
    EXPECT_NEAR(static_cast<double>(log.times[0]), first, 4.0);
    // Second flow starts after the drain at 2 GB/s: ~first + size/(2 GB/s).
    const double second =
        first + static_cast<double>(size) / bw_GBps_to_Bpns(2.0);
    EXPECT_NEAR(static_cast<double>(log.times[1]), second, 8.0);
    EXPECT_FALSE(tm->is_reconfiguring());
}
