/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/FoldEnumerator.hh"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <gtest/gtest.h>
#include <set>
#include <vector>

using AstraSim::Scheduling::FoldEnumerator;
using AstraSim::Scheduling::FoldVariant;
using AstraSim::Scheduling::ordered_factorizations;
using AstraSim::Scheduling::snake_coords;
using AstraSim::Scheduling::snake_end_gap;

static int l1(const std::vector<int>& a, const std::vector<int>& b) {
    int d = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        d += std::abs(a[i] - b[i]);
    }
    return d;
}

TEST(SnakeCoords, ConsecutiveAreOneHop_2x4) {
    std::vector<std::vector<int>> seq;
    for (int v = 0; v < 8; ++v) {
        seq.push_back(snake_coords(v, {4, 2}));
    }
    for (int v = 0; v + 1 < 8; ++v) {
        EXPECT_EQ(l1(seq[v], seq[v + 1]), 1) << v;
    }
    EXPECT_EQ(l1(seq[7], seq[0]), 1);  // closes: outer size 2 is even
    std::set<std::vector<int>> uniq(seq.begin(), seq.end());
    EXPECT_EQ(uniq.size(), 8u);
}

TEST(SnakeCoords, DoesNotCloseWhenOutermostOdd_2x3) {
    std::vector<std::vector<int>> seq;
    for (int v = 0; v < 6; ++v) {
        seq.push_back(snake_coords(v, {2, 3}));
    }
    for (int v = 0; v + 1 < 6; ++v) {
        EXPECT_EQ(l1(seq[v], seq[v + 1]), 1);
    }
    EXPECT_NE(l1(seq[5], seq[0]), 1);  // outer size 3 (odd): open
}

TEST(Enumerate, IdentityAlwaysPresentAndExact) {
    auto vs = FoldEnumerator::enumerate({2, 2, 2});
    ASSERT_FALSE(vs.empty());
    EXPECT_EQ(vs[0].footprint,
              (std::array<int, 3>{2, 2, 2}));  // identity first
    ASSERT_EQ(vs[0].embedding.size(), 8u);
    EXPECT_EQ(vs[0].embedding[0], (std::array<int, 3>{0, 0, 0}));
    EXPECT_EQ(vs[0].embedding[3], (std::array<int, 3>{1, 1, 0}));
    EXPECT_EQ(vs[0].embedding[7], (std::array<int, 3>{1, 1, 1}));
}

TEST(Enumerate, FoldsTpEightIntoSpareAxis_1x8x8) {
    auto vs = FoldEnumerator::enumerate({1, 8, 8});
    bool found = false;
    for (const auto& v : vs) {
        if (v.footprint == std::array<int, 3>{2, 4, 8}) {
            found = true;
            EXPECT_TRUE(v.ring_closes);
            ASSERT_EQ(v.embedding.size(), 64u);
            for (const auto& c : v.embedding) {
                EXPECT_GE(c[0], 0);
                EXPECT_LT(c[0], 2);
                EXPECT_GE(c[1], 0);
                EXPECT_LT(c[1], 4);
                EXPECT_GE(c[2], 0);
                EXPECT_LT(c[2], 8);
            }
        }
    }
    EXPECT_TRUE(found);
}

// Every emitted variant must be a bijection over its footprint cells.
TEST(Enumerate, VariantsAreBijectiveEmbeddings) {
    for (auto shape :
         std::vector<std::array<int, 3>>{{1, 8, 8}, {1, 6, 4}, {2, 2, 2}}) {
        auto vs = FoldEnumerator::enumerate(shape);
        int K = shape[0] * shape[1] * shape[2];
        for (const auto& v : vs) {
            ASSERT_EQ(static_cast<int>(v.embedding.size()), K);
            std::set<std::array<int, 3>> cells(v.embedding.begin(),
                                               v.embedding.end());
            EXPECT_EQ(static_cast<int>(cells.size()), K);
        }
    }
}

// A true 3-D job (no spare axis) folds via a JOINT fold: the donated "2" merges
// onto an axis owned by an even partner dim. 4x8x2 -> 4x4x4 (donate TP's 2 onto
// the PP=2 axis). Joint folds are defrag-only on a static torus: ring_closes is
// false (the donor ring's hairpin is not 1-hop without a full-axis wrap).
TEST(EnumerateJoint, FoldsTrue3D_4x8x2_to_4x4x4) {
    auto vs = FoldEnumerator::enumerate({4, 8, 2});
    bool found = false;
    for (const auto& v : vs) {
        if (v.footprint == std::array<int, 3>{4, 4, 4}) {
            found = true;
            EXPECT_FALSE(v.ring_closes);  // defrag-only joint fold
        }
    }
    EXPECT_TRUE(found);
}

// The odd-partner case is rejected: donating TP's 2 onto the PP=3 axis is not a
// legal even-partner joint fold, so 4x4x6 is never produced from 4x8x3.
TEST(EnumerateJoint, Rejects_4x8x3_to_4x4x6) {
    auto vs = FoldEnumerator::enumerate({4, 8, 3});
    for (const auto& v : vs) {
        EXPECT_NE(v.footprint, (std::array<int, 3>{4, 4, 6}));
    }
}

// A thin 3-D plate folds chunkier for defragmentation: 2x8x8 -> 4x4x8.
TEST(EnumerateJoint, ChunksThinPlate_2x8x8_to_4x4x8) {
    auto vs = FoldEnumerator::enumerate({2, 8, 8});
    bool found = false;
    for (const auto& v : vs) {
        if (v.footprint == std::array<int, 3>{4, 4, 8}) {
            found = true;
            EXPECT_FALSE(v.ring_closes);  // joint fold: defrag-only
        }
    }
    EXPECT_TRUE(found);
}

// Every emitted variant (identity, spare, and joint folds) must be a bijection
// from [0,K) onto distinct footprint cells -- the core safety property.
TEST(EnumerateJoint, AllVariantsBijectiveIncludingJoint) {
    std::vector<std::array<int, 3>> shapes = {{1, 8, 8}, {2, 8, 8}, {4, 8, 2},
                                              {4, 8, 3}, {1, 6, 4}, {4, 4, 4},
                                              {8, 4, 2}};
    for (auto shape : shapes) {
        auto vs = FoldEnumerator::enumerate(shape);
        int K = shape[0] * shape[1] * shape[2];
        for (const auto& v : vs) {
            ASSERT_EQ(static_cast<int>(v.embedding.size()), K);
            std::set<std::array<int, 3>> cells(v.embedding.begin(),
                                               v.embedding.end());
            EXPECT_EQ(static_cast<int>(cells.size()), K)
                << "non-bijective embedding for footprint " << v.footprint[0]
                << "x" << v.footprint[1] << "x" << v.footprint[2];
            // Also: each embedded coord must be within the footprint bounds.
            for (const auto& c : v.embedding) {
                for (int ax = 0; ax < 3; ++ax) {
                    EXPECT_GE(c[ax], 0);
                    EXPECT_LT(c[ax], v.footprint[ax]);
                }
            }
        }
    }
}

TEST(Snake1D, FindsClosedCycleThroughFreeNodes) {
    // 4x4x4 torus; free = a 2x2 face at z=0 -> ids form a 4-cycle.
    const int Dx = 4, Dy = 4, Dz = 4;
    std::vector<int> dims = {Dx, Dy, Dz};
    auto id = [&](int x, int y, int z) { return z * Dy * Dx + y * Dx + x; };
    std::vector<int> free = {id(0, 0, 0), id(1, 0, 0), id(1, 1, 0),
                             id(0, 1, 0)};
    auto cyc =
        FoldEnumerator::snake_1d(free, dims, /*len=*/4, /*max_depth=*/256);
    ASSERT_EQ(cyc.size(), 4u);
    // Every node distinct and from the free set.
    std::set<int> uniq(cyc.begin(), cyc.end());
    EXPECT_EQ(uniq.size(), 4u);
    std::set<int> fs(free.begin(), free.end());
    for (int n : cyc) {
        EXPECT_TRUE(fs.count(n));
    }
    // Consecutive (and wrap) are exactly 1 torus hop apart.
    auto hop = [&](int a, int b) {
        int ax = a % Dx, ay = (a / Dx) % Dy, az = a / (Dx * Dy);
        int bx = b % Dx, by = (b / Dx) % Dy, bz = b / (Dx * Dy);
        auto d1 = [&](int u, int w, int D) {
            int e = std::abs(u - w);
            return std::min(e, D - e);
        };
        return d1(ax, bx, Dx) + d1(ay, by, Dy) + d1(az, bz, Dz);
    };
    for (size_t i = 0; i < cyc.size(); ++i) {
        EXPECT_EQ(hop(cyc[i], cyc[(i + 1) % cyc.size()]), 1);
    }
}

TEST(Snake1D, ReturnsEmptyWhenNoCycleFits) {
    // Only 3 free nodes but len=4 -> impossible.
    std::vector<int> dims = {4, 4, 4};
    std::vector<int> free = {0, 1, 2};
    auto cyc =
        FoldEnumerator::snake_1d(free, dims, /*len=*/4, /*max_depth=*/256);
    EXPECT_TRUE(cyc.empty());
}

TEST(Snake1D, ReturnsEmptyWhenNoAdjacentCycle) {
    // 4 free nodes, all pairwise >= 2 hops apart: no closed 1-hop cycle exists
    // (exercises the topological-failure path, not just the size guard).
    const int Dx = 4, Dy = 4, Dz = 4;
    std::vector<int> dims = {Dx, Dy, Dz};
    auto id = [&](int x, int y, int z) { return z * Dy * Dx + y * Dx + x; };
    std::vector<int> free = {id(0, 0, 0), id(2, 0, 0), id(0, 2, 0),
                             id(2, 2, 0)};
    auto cyc =
        FoldEnumerator::snake_1d(free, dims, /*len=*/4, /*max_depth=*/256);
    EXPECT_TRUE(cyc.empty());
}

// Footprint dedup must keep the best PLAN per footprint, not the first one
// found. For (1,8,8) the DFS reaches footprint 2x8x4 first as an open joint
// fold (PP donated onto the TP-shared axis, discovered inside the 2x4x8
// hairpin's recursion) and previously discarded the direct PP hairpin with
// the same footprint -- demoting a perfectly 1-hop arrangement to class 2.
TEST(Enumerate, HairpinNotShadowedBySameFootprintJoint_1x8x8) {
    auto vs = FoldEnumerator::enumerate({1, 8, 8});
    bool found = false;
    for (const auto& v : vs) {
        if (v.footprint == std::array<int, 3>{2, 8, 4}) {
            found = true;
            EXPECT_TRUE(v.ring_closes)
                << "2x8x4 must survive as the PP hairpin, not the joint fold";
        }
    }
    EXPECT_TRUE(found);
}

// Same structure at small scale: (1,4,4) -> 2x4x2 is reachable both as the
// direct PP hairpin (closed) and as a joint fold inside the TP hairpin's
// recursion (open). The closed plan must win the footprint slot.
TEST(Enumerate, HairpinNotShadowedBySameFootprintJoint_1x4x4) {
    auto vs = FoldEnumerator::enumerate({1, 4, 4});
    bool found = false;
    for (const auto& v : vs) {
        if (v.footprint == std::array<int, 3>{2, 4, 2}) {
            found = true;
            EXPECT_TRUE(v.ring_closes);
        }
    }
    EXPECT_TRUE(found);
}

TEST(SnakeCoords, ConsecutiveAreOneHop_2x2x2) {
    std::vector<std::vector<int>> seq;
    for (int v = 0; v < 8; ++v) {
        seq.push_back(snake_coords(v, {2, 2, 2}));
    }
    for (int v = 0; v + 1 < 8; ++v) {
        EXPECT_EQ(l1(seq[v], seq[v + 1]), 1) << "step " << v;
    }
    EXPECT_EQ(l1(seq[7], seq[0]), 1);  // closes: outermost size 2
    std::set<std::vector<int>> uniq(seq.begin(), seq.end());
    EXPECT_EQ(uniq.size(), 8u);
}

TEST(SnakeCoords, ConsecutiveAreOneHop_4x3x2) {
    const int n = 24;
    std::vector<std::vector<int>> seq;
    for (int v = 0; v < n; ++v) {
        seq.push_back(snake_coords(v, {4, 3, 2}));
    }
    for (int v = 0; v + 1 < n; ++v) {
        EXPECT_EQ(l1(seq[v], seq[v + 1]), 1) << "step " << v;
    }
    EXPECT_EQ(l1(seq[n - 1], seq[0]), 1);  // closes: outermost size 2
    std::set<std::vector<int>> uniq(seq.begin(), seq.end());
    EXPECT_EQ(uniq.size(), static_cast<size_t>(n));
}

// Even-outermost: every inner digit reflects back to 0 at the end, so the
// end-to-start gap lies on the outermost axis alone (size-1 of it). This is
// the geometry the OCS wrap-around closure depends on.
TEST(SnakeCoords, EvenOutermostEndsSingleAxisGap_8x6x4) {
    auto end = snake_coords(8 * 6 * 4 - 1, {8, 6, 4});
    EXPECT_EQ(end[0], 0);
    EXPECT_EQ(end[1], 0);
    EXPECT_EQ(end[2], 3);  // gap = size-1 on the outermost axis only
}

// The fix arbitrates collisions; it must not create duplicate footprints or
// change which footprints are reachable.
TEST(Enumerate, FootprintsStayUniquePerVariant) {
    for (auto shape : std::vector<std::array<int, 3>>{{1, 8, 8},
                                                      {1, 4, 4},
                                                      {2, 8, 8},
                                                      {4, 8, 2},
                                                      {1, 6, 4},
                                                      {2, 4, 8}}) {
        auto vs = FoldEnumerator::enumerate(shape);
        std::set<std::array<int, 3>> fps;
        for (const auto& v : vs) {
            EXPECT_TRUE(fps.insert(v.footprint).second)
                << "duplicate footprint " << v.footprint[0] << "x"
                << v.footprint[1] << "x" << v.footprint[2];
        }
    }
}

TEST(OrderedFactorizations, EightMax3) {
    auto fs = ordered_factorizations(8, 3);
    std::set<std::vector<int>> got(fs.begin(), fs.end());
    std::set<std::vector<int>> want = {{8}, {2, 4}, {4, 2}, {2, 2, 2}};
    EXPECT_EQ(got, want);
}

TEST(OrderedFactorizations, TwelveMax3) {
    auto fs = ordered_factorizations(12, 3);
    std::set<std::vector<int>> got(fs.begin(), fs.end());
    std::set<std::vector<int>> want = {{12},   {2, 6},    {6, 2},    {3, 4},
                                       {4, 3}, {2, 2, 3}, {2, 3, 2}, {3, 2, 2}};
    EXPECT_EQ(got, want);
}

TEST(OrderedFactorizations, PrimeIsItself) {
    auto fs = ordered_factorizations(7, 3);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0], std::vector<int>({7}));
}

TEST(OrderedFactorizations, MaxPartsTwoExcludesTriples) {
    auto fs = ordered_factorizations(8, 2);
    std::set<std::vector<int>> got(fs.begin(), fs.end());
    std::set<std::vector<int>> want = {{8}, {2, 4}, {4, 2}};
    EXPECT_EQ(got, want);
}

TEST(SnakeEndGap, HairpinAndBoxesAndOdd) {
    EXPECT_EQ(snake_end_gap({4, 2}), 1);     // hairpin: closes
    EXPECT_EQ(snake_end_gap({8, 6, 2}), 1);  // even-outermost box: closes
    EXPECT_EQ(snake_end_gap({8, 4}), 3);     // single-axis OCS-closable gap
    EXPECT_EQ(snake_end_gap({2, 3}), 3);     // odd outermost: multi-axis open
}

// 96x1x1 was a snake-budget DEFER; multi-fold makes it a closing 8x6x2 box.
TEST(EnumerateMultiFold, Ring96HasClosingBoxVariant) {
    auto vs = FoldEnumerator::enumerate({96, 1, 1});
    bool found = false;
    for (const auto& v : vs) {
        std::array<int, 3> fp = v.footprint;
        std::sort(fp.begin(), fp.end());
        if (fp == std::array<int, 3>{2, 6, 8} && v.ring_closes) {
            found = true;
            // bijective over the box
            std::set<std::array<int, 3>> cells(v.embedding.begin(),
                                               v.embedding.end());
            EXPECT_EQ(cells.size(), 96u);
        }
    }
    EXPECT_TRUE(found);
}

// 2-D job needing a deep fold: 32x2x1 -> dim0 as (8,4) + dim1 on the third
// axis = 8x4x2. Unreachable today (one hairpin per dim).
TEST(EnumerateMultiFold, Dim32In2DJobFoldsToHalfTorusBox) {
    auto vs = FoldEnumerator::enumerate({32, 2, 1});
    bool found = false;
    for (const auto& v : vs) {
        std::array<int, 3> fp = v.footprint;
        std::sort(fp.begin(), fp.end());
        if (fp == std::array<int, 3>{2, 4, 8}) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(EnumerateMultiFold, OffFlagReproducesLegacySet) {
    auto legacy = FoldEnumerator::enumerate({96, 1, 1}, /*multifold=*/false);
    for (const auto& v : legacy) {
        // legacy: identity (96) or one hairpin (48x2) — never a 3-axis box
        int non_unit =
            (v.footprint[0] > 1) + (v.footprint[1] > 1) + (v.footprint[2] > 1);
        EXPECT_LE(non_unit, 2);
    }
}

TEST(EnumerateMultiFold, NewShapesStayBijective) {
    for (auto shape : std::vector<std::array<int, 3>>{
             {96, 1, 1}, {32, 2, 1}, {24, 1, 1}, {1, 12, 1}, {10, 4, 1}}) {
        auto vs = FoldEnumerator::enumerate(shape);
        int K = shape[0] * shape[1] * shape[2];
        for (const auto& v : vs) {
            ASSERT_EQ(static_cast<int>(v.embedding.size()), K);
            std::set<std::array<int, 3>> cells(v.embedding.begin(),
                                               v.embedding.end());
            EXPECT_EQ(static_cast<int>(cells.size()), K);
            for (const auto& c : v.embedding) {
                for (int ax = 0; ax < 3; ++ax) {
                    EXPECT_GE(c[ax], 0);
                    EXPECT_LT(c[ax], v.footprint[ax]);
                }
            }
        }
    }
}

TEST(EnumerateMultiFold, VariantCountCapped) {
    auto vs = FoldEnumerator::enumerate({512, 1, 1});
    EXPECT_LE(vs.size(), 512u);
    EXPECT_TRUE(vs[0].identity);  // identity survives the cap (it is first)
}

// Footprint-collision arbitration: for {4,4,2}, grow's joint fold reaches
// footprint 2x4x4 as an OPEN plan; the exclusive enumerator reaches the same
// footprint as a rotated identity (every dim on its own axis), which CLOSES.
// Strictly better comm class displaces the incumbent — intended drift.
TEST(EnumerateMultiFold, RotatedIdentityDisplacesOpenJointFold) {
    bool found_multi = false;
    for (const auto& v : FoldEnumerator::enumerate({4, 4, 2})) {
        if (v.footprint == std::array<int, 3>{2, 4, 4}) {
            found_multi = true;
            EXPECT_TRUE(v.ring_closes) << "closed plan must win the slot";
        }
    }
    EXPECT_TRUE(found_multi);
    bool found_legacy = false;
    for (const auto& v :
         FoldEnumerator::enumerate({4, 4, 2}, /*multifold=*/false)) {
        if (v.footprint == std::array<int, 3>{2, 4, 4}) {
            found_legacy = true;
            EXPECT_FALSE(v.ring_closes) << "legacy joint fold is open";
        }
    }
    EXPECT_TRUE(found_legacy);
}
