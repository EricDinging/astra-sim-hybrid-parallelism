/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/FailureSelection.hh"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

using AstraSim::Scheduling::select_failed_npus;

// Exact count = round(prob * N).
TEST(FailureSelection, ExactCount) {
    EXPECT_EQ(select_failed_npus(1000, 0.01, 42).size(), 10u);
    EXPECT_EQ(select_failed_npus(512, 0.05, 42).size(), 26u);  // round(25.6)
    EXPECT_TRUE(select_failed_npus(1000, 0.0, 42).empty());
    EXPECT_EQ(select_failed_npus(64, 1.0, 42).size(), 64u);
}

// Ids are distinct, in range, and ascending.
TEST(FailureSelection, DistinctInRangeSorted) {
    std::vector<int> f = select_failed_npus(1000, 0.1, 42);
    ASSERT_EQ(f.size(), 100u);
    EXPECT_TRUE(std::is_sorted(f.begin(), f.end()));
    std::set<int> uniq(f.begin(), f.end());
    EXPECT_EQ(uniq.size(), f.size());
    EXPECT_GE(f.front(), 0);
    EXPECT_LT(f.back(), 1000);
}

// Same (N, prob, seed) -> identical result; different seed -> (very likely)
// different result.
TEST(FailureSelection, Reproducible) {
    EXPECT_EQ(select_failed_npus(1000, 0.1, 42),
              select_failed_npus(1000, 0.1, 42));
    EXPECT_NE(select_failed_npus(1000, 0.1, 42),
              select_failed_npus(1000, 0.1, 7));
}

// Out-of-range probabilities clamp to [0, 1].
TEST(FailureSelection, ClampsProbability) {
    EXPECT_TRUE(select_failed_npus(100, -0.5, 42).empty());
    EXPECT_EQ(select_failed_npus(100, 2.0, 42).size(), 100u);
}

// Degenerate cluster sizes are safe.
TEST(FailureSelection, DegenerateSizes) {
    EXPECT_TRUE(select_failed_npus(0, 0.5, 42).empty());
    EXPECT_TRUE(select_failed_npus(-5, 0.5, 42).empty());
}
