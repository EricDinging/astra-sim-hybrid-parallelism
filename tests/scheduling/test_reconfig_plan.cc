/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/ReconfigPlan.hh"

#include <gtest/gtest.h>

using namespace AstraSim::Scheduling;

TEST(ReconfigPlan, DefaultEmpty) {
    PlacementResult r;
    EXPECT_TRUE(r.reconfig_plan.empty());
}

TEST(ReconfigPlan, NonEmptyWhenPopulated) {
    ReconfigPlan p;
    p.ocs_edges.push_back({0, 7});
    p.routes.push_back(RouteRow{0, 7, {0, 7}});
    EXPECT_FALSE(p.empty());
}
