/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

// Verifies CommunicatorGroup's opt-in order preservation (Decision D4). The
// constructor only stores members and conditionally sorts; it does not touch
// the generator, so a nullptr generator is safe for this construction-only
// test.

#include "astra-sim/system/CommunicatorGroup.hh"

#include <gtest/gtest.h>
#include <vector>

TEST(CommunicatorGroupOrder, SortsByDefault) {
    std::vector<int> npus = {20, 21, 37, 36};
    AstraSim::CommunicatorGroup cg(/*id=*/1, npus, /*generator=*/nullptr);
    EXPECT_EQ(cg.involved_NPUs, (std::vector<int>{20, 21, 36, 37}));
}

TEST(CommunicatorGroupOrder, PreservesOrderWhenRequested) {
    std::vector<int> npus = {20, 21, 37, 36};
    AstraSim::CommunicatorGroup cg(/*id=*/1, npus, /*generator=*/nullptr,
                                   /*preserve_order=*/true);
    EXPECT_EQ(cg.involved_NPUs, (std::vector<int>{20, 21, 37, 36}));
}
