/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/FragmentationScorer.hh"

#include <array>
#include <gtest/gtest.h>
#include <vector>

namespace {
using AstraSim::Scheduling::Compactness;
using AstraSim::Scheduling::FewestBlocksTouched;
using AstraSim::Scheduling::make_fragmentation_scorer;
using AstraSim::Scheduling::parse_block_size;
using AstraSim::Scheduling::ScoredPlacement;

inline int id(int x, int y, int z, int Dx, int Dy) {
    return z * Dy * Dx + y * Dx + x;
}
}  // namespace

TEST(ParseBlockSize, ParsesCubicAndNonCubic) {
    std::array<int, 3> b{};
    ASSERT_TRUE(parse_block_size("4x4x4", b));
    EXPECT_EQ(b, (std::array<int, 3>{4, 4, 4}));
    ASSERT_TRUE(parse_block_size("2x4x1", b));
    EXPECT_EQ(b, (std::array<int, 3>{2, 4, 1}));
    EXPECT_FALSE(parse_block_size("4x4", b));
    EXPECT_FALSE(parse_block_size("axbxc", b));
    EXPECT_FALSE(parse_block_size("", b));
    EXPECT_FALSE(parse_block_size("4x4x4x5", b));
    EXPECT_FALSE(parse_block_size("4x-1x4", b));
    EXPECT_FALSE(parse_block_size("4x0x4", b));
}

TEST(FewestBlocksTouched, AlignedPlacementTouchesFewerBlocks) {
    const int Dx = 8, Dy = 8, Dz = 8;
    std::vector<int> dims = {Dx, Dy, Dz};
    FewestBlocksTouched scorer({4, 4, 4});

    // A 4x2x1 footprint fully inside block (0,0,0): x in [0,4), y in [0,2).
    std::vector<int> aligned;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            aligned.push_back(id(x, y, 0, Dx, Dy));
        }
    }
    ScoredPlacement pa{&aligned, &dims, {4, 2, 1}};

    // Same shape straddling the x block boundary: x in [2,6) crosses x=4.
    std::vector<int> straddle;
    for (int y = 0; y < 2; ++y) {
        for (int x = 2; x < 6; ++x) {
            straddle.push_back(id(x, y, 0, Dx, Dy));
        }
    }
    ScoredPlacement ps{&straddle, &dims, {4, 2, 1}};

    EXPECT_LT(scorer.cost(pa), scorer.cost(ps));  // 1 block < 2 blocks
}

TEST(Compactness, PrefersCubicFootprint) {
    const int Dx = 16, Dy = 16, Dz = 16;
    std::vector<int> dims = {Dx, Dy, Dz};
    std::vector<int> dummy = {0};
    Compactness scorer;
    ScoredPlacement thin{&dummy, &dims, {2, 8, 8}};    // max dim 8
    ScoredPlacement chunky{&dummy, &dims, {4, 4, 8}};  // max dim 8 ... tie
    ScoredPlacement cube{&dummy, &dims, {4, 4, 4}};    // max dim 4
    EXPECT_LT(scorer.cost(cube), scorer.cost(thin));
    EXPECT_EQ(scorer.cost(chunky), scorer.cost(thin));
}

TEST(Factory, KnownAndUnknown) {
    EXPECT_NE(make_fragmentation_scorer("fewest-blocks", {4, 4, 4}), nullptr);
    EXPECT_NE(make_fragmentation_scorer("compactness", {4, 4, 4}), nullptr);
    EXPECT_EQ(make_fragmentation_scorer("nope", {4, 4, 4}), nullptr);
    // deleted with the RDCN model (OCS links cost nothing)
    EXPECT_EQ(make_fragmentation_scorer("fewest-blocks-ocs", {4, 4, 4}),
              nullptr);
}
