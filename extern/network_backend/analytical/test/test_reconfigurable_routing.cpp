/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

// Unit tests for the on-demand DOR Router (reconfigurable backend). The bar is
// bit-identical to the old eager precomputeRoutes_DOR() table: compute_dor()
// must equal an independent DOR oracle, lookup() must equal compute_dor(),
// overrides take precedence, and a bounded cache never changes results.

#include <gtest/gtest.h>

#include <unordered_set>
#include <vector>

#include "reconfigurable/Router.h"
#include "reconfigurable/Topology.h"

using namespace NetworkAnalyticalReconfigurable;

namespace {

// Dx=Dy=Dz=4 torus, id = z*16 + y*4 + x (x fastest), matching the backend.
constexpr int D = 4;
constexpr int N = D * D * D;

std::vector<int> id_to_coord(int id) {
    return {id % D, (id / D) % D, id / (D * D)};
}

// Independent unidirectional-DOR oracle: align dim 0 (+1 mod D), then dim 1,
// then dim 2. Mirrors the baseline arc selection (bidi=false).
std::vector<int> dor_oracle(int src, int dst) {
    auto cur = id_to_coord(src);
    const auto dstc = id_to_coord(dst);
    std::vector<int> ids = {src};
    if (src == dst) {
        return ids;
    }
    for (int d = 0; d < 3; ++d) {
        while (cur[d] != dstc[d]) {
            cur[d] = (cur[d] + 1) % D;
            ids.push_back(cur[2] * D * D + cur[1] * D + cur[0]);
        }
    }
    return ids;
}

std::vector<int> route_ids(const Route& r) {
    std::vector<int> ids;
    for (const auto& dev : r) {
        ids.push_back(dev->get_id());
    }
    return ids;
}

Router make_router(const std::unordered_set<int>* failed = nullptr, std::size_t budget = Router::kDefaultBudgetBytes) {
    auto topo = std::make_shared<Topology>(N, N);
    return Router(topo, {D, D, D}, /*is_torus=*/true, /*bidi=*/false, N, failed, budget);
}

}  // namespace

// compute_dor() == independent oracle for every pair on the idle torus.
TEST(RouterDor, MatchesOracleAllPairs) {
    Router router = make_router();
    for (int s = 0; s < N; ++s) {
        for (int t = 0; t < N; ++t) {
            EXPECT_EQ(route_ids(router.compute_dor(s, t)), dor_oracle(s, t)) << "src=" << s << " dst=" << t;
        }
    }
}

// Every route starts at src, ends at dst, and each hop is a single-dim torus
// step (exactly one coordinate changes, by +/-1 mod D).
TEST(RouterDor, StructurallyValid) {
    Router router = make_router();
    for (int s = 0; s < N; ++s) {
        for (int t = 0; t < N; ++t) {
            const auto ids = route_ids(router.compute_dor(s, t));
            ASSERT_FALSE(ids.empty());
            EXPECT_EQ(ids.front(), s);
            EXPECT_EQ(ids.back(), t);
            for (std::size_t i = 1; i < ids.size(); ++i) {
                const auto a = id_to_coord(ids[i - 1]);
                const auto b = id_to_coord(ids[i]);
                int changed = 0;
                for (int d = 0; d < 3; ++d) {
                    if (a[d] != b[d]) {
                        ++changed;
                        const int diff = (b[d] - a[d] + D) % D;
                        EXPECT_TRUE(diff == 1 || diff == D - 1);
                    }
                }
                EXPECT_EQ(changed, 1) << "non-single-dim hop " << ids[i - 1] << "->" << ids[i];
            }
        }
    }
}

// lookup() returns exactly compute_dor() when no override is installed.
TEST(RouterCache, LookupEqualsComputeDor) {
    Router router = make_router();
    for (int s = 0; s < N; ++s) {
        for (int t = 0; t < N; ++t) {
            EXPECT_EQ(route_ids(router.lookup(s, t)), route_ids(router.compute_dor(s, t)));
        }
    }
}

// Override takes precedence; clear() restores pure DOR.
TEST(RouterOverride, PrecedenceAndClear) {
    Router router = make_router();
    auto topo = std::make_shared<Topology>(N, N);
    // A bogus 2-hop override 0 -> 63 that DOR would never produce.
    Route custom;
    custom.push_back(topo->get_device(0));
    custom.push_back(topo->get_device(63));
    router.set_override(0, 63, custom);

    EXPECT_EQ(route_ids(router.lookup(0, 63)), (std::vector<int>{0, 63}));
    EXPECT_EQ(router.override_count(), 1u);
    // A different pair is unaffected (still DOR).
    EXPECT_EQ(route_ids(router.lookup(1, 5)), dor_oracle(1, 5));

    router.clear();
    EXPECT_EQ(router.override_count(), 0u);
    EXPECT_EQ(route_ids(router.lookup(0, 63)), dor_oracle(0, 63));
}

// A tiny budget bounds the cache, and eviction never changes a returned route.
TEST(RouterCache, BoundedEvictionKeepsResultsCorrect) {
    Router router = make_router(nullptr, /*budget=*/512);  // ~ a couple of routes
    for (int s = 0; s < N; ++s) {
        for (int t = 0; t < N; ++t) {
            EXPECT_EQ(route_ids(router.lookup(s, t)), dor_oracle(s, t));
        }
    }
    EXPECT_LE(router.cache_bytes(), 512u);
    EXPECT_LT(router.cache_size(), static_cast<std::size_t>(N * N));  // actually evicting
    // Re-lookups after heavy eviction are still correct.
    EXPECT_EQ(route_ids(router.lookup(0, 63)), dor_oracle(0, 63));
}

// With a failed intermediate node, the route avoids it, keeps valid endpoints,
// and visits only alive nodes (exact detour path covered by integration diff).
TEST(RouterFailed, DetoursAroundFailedNode) {
    std::unordered_set<int> failed = {1};  // direct hop on 0 -> 2 (x-axis)
    Router router = make_router(&failed);
    const auto ids = route_ids(router.compute_dor(0, 2));
    ASSERT_GE(ids.size(), 2u);
    EXPECT_EQ(ids.front(), 0);
    EXPECT_EQ(ids.back(), 2);
    for (int id : ids) {
        EXPECT_EQ(failed.count(id), 0u) << "route traverses failed node " << id;
    }
}
