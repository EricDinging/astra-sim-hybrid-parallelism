/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/BlockTiler.hh"

#include <array>
#include <gtest/gtest.h>

using namespace AstraSim::Scheduling;

TEST(BlockTiler, FullBlockFootprint) {
    auto t = tile({8, 8, 4}, {4, 4, 4});
    EXPECT_EQ(t.grid_dims, (std::array<int, 3>{2, 2, 1}));
    ASSERT_EQ(t.blocks.size(), 4u);
    for (const auto& b : t.blocks) {
        EXPECT_EQ(b.shape, (std::array<int, 3>{4, 4, 4}));
    }
    EXPECT_EQ(t.blocks[0].fp_offset, (std::array<int, 3>{0, 0, 0}));
    EXPECT_EQ(t.blocks[1].fp_offset, (std::array<int, 3>{4, 0, 0}));
    EXPECT_EQ(t.blocks[2].fp_offset, (std::array<int, 3>{0, 4, 0}));
    EXPECT_EQ(t.blocks[3].fp_offset, (std::array<int, 3>{4, 4, 0}));
    EXPECT_EQ(t.offset_free, (std::array<bool, 3>{false, false, false}));
}

TEST(BlockTiler, SlabFootprintHasFreeOffset) {
    auto t = tile({8, 8, 1}, {4, 4, 4});
    EXPECT_EQ(t.grid_dims, (std::array<int, 3>{2, 2, 1}));
    ASSERT_EQ(t.blocks.size(), 4u);
    for (const auto& b : t.blocks) {
        EXPECT_EQ(b.shape, (std::array<int, 3>{4, 4, 1}));
    }
    EXPECT_EQ(t.blocks[3].fp_offset, (std::array<int, 3>{4, 4, 0}));
    EXPECT_EQ(t.offset_free, (std::array<bool, 3>{false, false, true}));
}

TEST(BlockTiler, BoundaryPartialBlock) {
    auto t = tile({6, 4, 4}, {4, 4, 4});  // x: 6 -> segments 4 and 2
    EXPECT_EQ(t.grid_dims, (std::array<int, 3>{2, 1, 1}));
    ASSERT_EQ(t.blocks.size(), 2u);
    EXPECT_EQ(t.blocks[0].grid, (std::array<int, 3>{0, 0, 0}));
    EXPECT_EQ(t.blocks[0].shape[0], 4);
    EXPECT_EQ(t.blocks[1].grid, (std::array<int, 3>{1, 0, 0}));
    EXPECT_EQ(t.blocks[1].shape[0], 2);
    EXPECT_EQ(t.blocks[1].fp_offset[0], 4);
    EXPECT_FALSE(t.offset_free[0]);  // x>block -> multi-segment, no DOF
}

TEST(BlockTiler, ExactFitFootprint) {
    auto t = tile({4, 4, 4},
                  {4, 4, 4});  // each dim == block dim: one full block, no DOF
    EXPECT_EQ(t.grid_dims, (std::array<int, 3>{1, 1, 1}));
    ASSERT_EQ(t.blocks.size(), 1u);
    EXPECT_EQ(t.blocks[0].shape, (std::array<int, 3>{4, 4, 4}));
    EXPECT_EQ(t.blocks[0].fp_offset, (std::array<int, 3>{0, 0, 0}));
    EXPECT_EQ(t.offset_free, (std::array<bool, 3>{false, false, false}));
}

TEST(BlockTiler, NonCubicBlock) {
    // 4x4x2 block tiling an 8x8x4 footprint -> a 2x2x2 grid of 4x4x2 blocks.
    auto t = tile({8, 8, 4}, {4, 4, 2});
    EXPECT_EQ(t.grid_dims, (std::array<int, 3>{2, 2, 2}));
    ASSERT_EQ(t.blocks.size(), 8u);
    for (const auto& b : t.blocks) {
        EXPECT_EQ(b.shape, (std::array<int, 3>{4, 4, 2}));
    }
    EXPECT_EQ(t.offset_free, (std::array<bool, 3>{false, false, false}));
}
