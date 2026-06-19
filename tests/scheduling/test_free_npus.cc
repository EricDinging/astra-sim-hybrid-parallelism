/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/FreeNpus.hh"

#include <gtest/gtest.h>

#include <unordered_set>
#include <vector>

using AstraSim::Scheduling::free_npus_excluding;

TEST(FreeNpus, ExcludesBusyAndFailedAscending) {
    std::unordered_set<int> busy = {1, 4};
    std::unordered_set<int> failed = {2, 4};  // 4 is both; counted once
    EXPECT_EQ(free_npus_excluding(6, busy, failed),
              (std::vector<int>{0, 3, 5}));
}

TEST(FreeNpus, NoExclusionsReturnsAll) {
    EXPECT_EQ(free_npus_excluding(3, {}, {}), (std::vector<int>{0, 1, 2}));
}

TEST(FreeNpus, AllExcludedReturnsEmpty) {
    std::unordered_set<int> failed = {0, 1, 2};
    EXPECT_TRUE(free_npus_excluding(3, {}, failed).empty());
}

TEST(FreeNpus, ZeroTotalReturnsEmpty) {
    EXPECT_TRUE(free_npus_excluding(0, {}, {}).empty());
}
