/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include <astra-network-analytical/common/EventQueue.h>
#include <astra-network-analytical/reconfigurable/Device.h>
#include <astra-network-analytical/reconfigurable/Link.h>
#include <astra-network-analytical/reconfigurable/Topology.h>
#include <astra-network-analytical/reconfigurable/TopologyManager.h>

#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <vector>

using namespace NetworkAnalytical;
using namespace NetworkAnalyticalReconfigurable;

TEST(TopologyManagerWiring, InstallsRouteAndBandwidth) {
    auto eq = std::make_shared<EventQueue>();
    Topology::set_event_queue(eq);
    std::map<int, std::vector<BandwidthRow>> bw;
    std::map<int, std::vector<LatencyRow>> lt;
    TopologyManager tm(8, 8, eq.get(), bw, lt, {2, 2, 2}, true);

    tm.apply_job_wiring({{0, 7}}, {{0, 7}, {7, 0}}, Bandwidth(50),
                        Latency(500));

    const auto& r = tm.get_precomputed_route(0, 7);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r.front()->get_id(), 0);
    EXPECT_EQ(r.back()->get_id(), 7);
    EXPECT_EQ(tm.get_device(0)->get_link(7)->get_bandwidth(), Bandwidth(50));
    EXPECT_EQ(tm.get_device(7)->get_link(0)->get_bandwidth(), Bandwidth(50));
}
