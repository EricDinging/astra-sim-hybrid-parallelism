/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

// Unit tests for astra-sim/scheduling/FirstFit.
// All tests construct synthetic JobInstance + ClusterView objects in code and
// exercise the policy through its public try_place() API. The anonymous-
// namespace helpers inside FirstFit.cc are deliberately not exposed.

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/FirstFit.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <numeric>
#include <unordered_set>
#include <vector>

namespace {

using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::FirstFit;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::PlacementOutcome;
using AstraSim::Scheduling::PlacementResult;

// X-fastest linearization: matches FirstFit.cc's coord_to_id and the
// TopologyManager.cpp:299-306 convention. Helper used by every test case.
inline int npu_id(int x, int y, int z, int Dx, int Dy) {
    return z * Dy * Dx + y * Dx + x;
}

// Build a ClusterView with every NPU free.
ClusterView make_full_cluster(int Dx, int Dy, int Dz) {
    const int total = Dx * Dy * Dz;
    std::vector<int> free_npus(total);
    std::iota(free_npus.begin(), free_npus.end(), 0);
    std::vector<int> dims = {Dx, Dy, Dz};
    return ClusterView(std::move(free_npus), std::move(dims), total,
                       /*current_tick=*/0);
}

// Build a JobInstance with the requested shape and num_ranks. Trace dir is
// empty and runtime is nullptr — both safe (verified during plan-writing).
JobInstance make_job(int job_id, int num_ranks, std::array<int, 3> shape) {
    JobArrival arrival{job_id, /*arrival_time=*/0, num_ranks, shape};
    return JobInstance(arrival, /*trace_dir=*/"", /*runtime=*/nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------
// Case 1: empty cluster, non-wrapping placement.
// Anchor (0,0,0) for a shape that fits well inside the cluster. Verifies the
// baseline PLACED outcome and that rank ordering follows the DP-fastest,
// x-fastest convention.
// ---------------------------------------------------------------------------
TEST(FirstFitBaseline, EmptyClusterNonWrappingPlacement) {
    const int Dx = 4, Dy = 4, Dz = 4;
    ClusterView view = make_full_cluster(Dx, Dy, Dz);
    JobInstance job = make_job(/*job_id=*/1, /*num_ranks=*/8,
                               /*shape=*/{2, 2, 2});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    ASSERT_EQ(result.outcome, PlacementOutcome::PLACED);
    ASSERT_EQ(result.npus.size(), 8u);

    // Expected layout with anchor (0,0,0), ax = (0,1,2):
    //   rank i = pp*TP*DP + tp*DP + dp = pp*4 + tp*2 + dp
    //   coord  = (dp, tp, pp) = (0..1, 0..1, 0..1)
    EXPECT_EQ(result.npus[0], npu_id(0, 0, 0, Dx, Dy));
    EXPECT_EQ(result.npus[1], npu_id(1, 0, 0, Dx, Dy));
    EXPECT_EQ(result.npus[2], npu_id(0, 1, 0, Dx, Dy));
    EXPECT_EQ(result.npus[3], npu_id(1, 1, 0, Dx, Dy));
    EXPECT_EQ(result.npus[4], npu_id(0, 0, 1, Dx, Dy));
    EXPECT_EQ(result.npus[5], npu_id(1, 0, 1, Dx, Dy));
    EXPECT_EQ(result.npus[6], npu_id(0, 1, 1, Dx, Dy));
    EXPECT_EQ(result.npus[7], npu_id(1, 1, 1, Dx, Dy));
}

// ---------------------------------------------------------------------------
// Case 2: wrap on x only.
// Cluster 4x4x4. Free NPUs at (y=0, z=0) are x=2, x=3, x=0 only. Shape
// (3,1,1) — no in-bounds 3-run fits along x (x=0,1,2 has x=1 busy; x=1,2,3
// has x=1 busy), but the wrapped run x=2,3,0 fits.
// ---------------------------------------------------------------------------
TEST(FirstFitWrap, WrapsOnXAxis) {
    const int Dx = 4, Dy = 4, Dz = 4;
    std::vector<int> dims = {Dx, Dy, Dz};
    std::vector<int> free_npus = {
        npu_id(2, 0, 0, Dx, Dy),
        npu_id(3, 0, 0, Dx, Dy),
        npu_id(0, 0, 0, Dx, Dy),
    };
    ClusterView view(free_npus, dims, /*total=*/Dx * Dy * Dz,
                     /*current_tick=*/0);
    JobInstance job = make_job(/*job_id=*/1, /*num_ranks=*/3,
                               /*shape=*/{3, 1, 1});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    ASSERT_EQ(result.outcome, PlacementOutcome::PLACED);
    ASSERT_EQ(result.npus.size(), 3u);
    EXPECT_EQ(result.npus[0], npu_id(2, 0, 0, Dx, Dy));
    EXPECT_EQ(result.npus[1], npu_id(3, 0, 0, Dx, Dy));
    EXPECT_EQ(result.npus[2], npu_id(0, 0, 0, Dx, Dy));  // wrap: (3+1)%4=0
}

// ---------------------------------------------------------------------------
// Case 3: wrap on y only.
// Same construction as case 2, rotated to the y axis. Shape (1,3,1) with
// free NPUs at (x=0, y=2,3,0, z=0).
// ---------------------------------------------------------------------------
TEST(FirstFitWrap, WrapsOnYAxis) {
    const int Dx = 4, Dy = 4, Dz = 4;
    std::vector<int> dims = {Dx, Dy, Dz};
    std::vector<int> free_npus = {
        npu_id(0, 2, 0, Dx, Dy),
        npu_id(0, 3, 0, Dx, Dy),
        npu_id(0, 0, 0, Dx, Dy),
    };
    ClusterView view(free_npus, dims, Dx * Dy * Dz, 0);
    JobInstance job = make_job(/*job_id=*/2, /*num_ranks=*/3,
                               /*shape=*/{1, 3, 1});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    ASSERT_EQ(result.outcome, PlacementOutcome::PLACED);
    ASSERT_EQ(result.npus.size(), 3u);
    // With ax = (0,1,2), placed_dims = (1, 3, 1). Anchor (0, 2, 0) wins:
    //   rank 0: (0, 2, 0); rank 1: (0, 3, 0); rank 2: (0, 0, 0) [wrap].
    EXPECT_EQ(result.npus[0], npu_id(0, 2, 0, Dx, Dy));
    EXPECT_EQ(result.npus[1], npu_id(0, 3, 0, Dx, Dy));
    EXPECT_EQ(result.npus[2], npu_id(0, 0, 0, Dx, Dy));
}

// ---------------------------------------------------------------------------
// Case 4: wrap on z only.
// ---------------------------------------------------------------------------
TEST(FirstFitWrap, WrapsOnZAxis) {
    const int Dx = 4, Dy = 4, Dz = 4;
    std::vector<int> dims = {Dx, Dy, Dz};
    std::vector<int> free_npus = {
        npu_id(0, 0, 2, Dx, Dy),
        npu_id(0, 0, 3, Dx, Dy),
        npu_id(0, 0, 0, Dx, Dy),
    };
    ClusterView view(free_npus, dims, Dx * Dy * Dz, 0);
    JobInstance job = make_job(/*job_id=*/3, /*num_ranks=*/3,
                               /*shape=*/{1, 1, 3});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    ASSERT_EQ(result.outcome, PlacementOutcome::PLACED);
    ASSERT_EQ(result.npus.size(), 3u);
    // Anchor (0, 0, 2) wins:
    //   rank 0: (0, 0, 2); rank 1: (0, 0, 3); rank 2: (0, 0, 0) [wrap].
    EXPECT_EQ(result.npus[0], npu_id(0, 0, 2, Dx, Dy));
    EXPECT_EQ(result.npus[1], npu_id(0, 0, 3, Dx, Dy));
    EXPECT_EQ(result.npus[2], npu_id(0, 0, 0, Dx, Dy));
}

// ---------------------------------------------------------------------------
// Case 5: wrap on two axes simultaneously (x and y).
// Cluster 4x4x4. Construct free NPUs forming a 2x2x1 box at (x in {3, 0},
// y in {3, 0}, z = 0). Shape (2, 2, 1) wraps on both x and y when anchor is
// (3, 3, 0).
// ---------------------------------------------------------------------------
TEST(FirstFitWrap, WrapsOnTwoAxesXY) {
    const int Dx = 4, Dy = 4, Dz = 4;
    std::vector<int> dims = {Dx, Dy, Dz};
    std::vector<int> free_npus = {
        npu_id(3, 3, 0, Dx, Dy),
        npu_id(0, 3, 0, Dx, Dy),
        npu_id(3, 0, 0, Dx, Dy),
        npu_id(0, 0, 0, Dx, Dy),
    };
    ClusterView view(free_npus, dims, Dx * Dy * Dz, 0);
    JobInstance job = make_job(/*job_id=*/4, /*num_ranks=*/4,
                               /*shape=*/{2, 2, 1});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    ASSERT_EQ(result.outcome, PlacementOutcome::PLACED);
    ASSERT_EQ(result.npus.size(), 4u);
    // Expected layout when anchor=(3,3,0), ax=(0,1,2):
    //   rank 0: (3,3,0); rank 1: (0,3,0); rank 2: (3,0,0); rank 3: (0,0,0)
    EXPECT_EQ(result.npus[0], npu_id(3, 3, 0, Dx, Dy));
    EXPECT_EQ(result.npus[1], npu_id(0, 3, 0, Dx, Dy));
    EXPECT_EQ(result.npus[2], npu_id(3, 0, 0, Dx, Dy));
    EXPECT_EQ(result.npus[3], npu_id(0, 0, 0, Dx, Dy));
}

// ---------------------------------------------------------------------------
// Case 6: wrap on three axes simultaneously (x, y, z).
// Cluster 4x4x4. Free NPUs form a 2x2x2 corner-and-wrap box.
// ---------------------------------------------------------------------------
TEST(FirstFitWrap, WrapsOnThreeAxes) {
    const int Dx = 4, Dy = 4, Dz = 4;
    std::vector<int> dims = {Dx, Dy, Dz};
    std::vector<int> free_npus;
    for (int z : {3, 0}) {
        for (int y : {3, 0}) {
            for (int x : {3, 0}) {
                free_npus.push_back(npu_id(x, y, z, Dx, Dy));
            }
        }
    }
    ClusterView view(free_npus, dims, Dx * Dy * Dz, 0);
    JobInstance job = make_job(/*job_id=*/5, /*num_ranks=*/8,
                               /*shape=*/{2, 2, 2});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    ASSERT_EQ(result.outcome, PlacementOutcome::PLACED);
    ASSERT_EQ(result.npus.size(), 8u);
    // Anchor (3,3,3), ax=(0,1,2). DP fastest (x), then TP (y), then PP (z).
    // rank index i = pp*4 + tp*2 + dp; coords decode under modulo.
    EXPECT_EQ(result.npus[0], npu_id(3, 3, 3, Dx, Dy));
    EXPECT_EQ(result.npus[1], npu_id(0, 3, 3, Dx, Dy));
    EXPECT_EQ(result.npus[2], npu_id(3, 0, 3, Dx, Dy));
    EXPECT_EQ(result.npus[3], npu_id(0, 0, 3, Dx, Dy));
    EXPECT_EQ(result.npus[4], npu_id(3, 3, 0, Dx, Dy));
    EXPECT_EQ(result.npus[5], npu_id(0, 3, 0, Dx, Dy));
    EXPECT_EQ(result.npus[6], npu_id(3, 0, 0, Dx, Dy));
    EXPECT_EQ(result.npus[7], npu_id(0, 0, 0, Dx, Dy));
}

// ---------------------------------------------------------------------------
// Case 7: de-dup rule when shape[i] == D_i.
// Job (4, 1, 1) fully occupies the x axis. With the de-dup, anchor x is
// pinned to 0; result NPUs are x = 0,1,2,3 at (y=0, z=0).
// ---------------------------------------------------------------------------
TEST(FirstFitWrap, DedupWhenShapeEqualsDim) {
    const int Dx = 4, Dy = 4, Dz = 4;
    ClusterView view = make_full_cluster(Dx, Dy, Dz);
    JobInstance job = make_job(/*job_id=*/6, /*num_ranks=*/4,
                               /*shape=*/{Dx, 1, 1});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    ASSERT_EQ(result.outcome, PlacementOutcome::PLACED);
    ASSERT_EQ(result.npus.size(), 4u);
    EXPECT_EQ(result.npus[0], npu_id(0, 0, 0, Dx, Dy));
    EXPECT_EQ(result.npus[1], npu_id(1, 0, 0, Dx, Dy));
    EXPECT_EQ(result.npus[2], npu_id(2, 0, 0, Dx, Dy));
    EXPECT_EQ(result.npus[3], npu_id(3, 0, 0, Dx, Dy));
}

// ---------------------------------------------------------------------------
// Case 8: DEFER when current state can't host even with wrap.
// Cluster 4x4x4. Shape (3,1,1). Free NPUs at (y=0,z=0) are only x=0 and
// x=2 — no run of 3 fits along x (wrapped or not). Other (y,z) rows are
// entirely empty. All rotations also fail (e.g., shape (1,3,1) along y
// would need 3 free NPUs in a y-column at some (x,z); none exist).
// ---------------------------------------------------------------------------
TEST(FirstFitWrap, DeferWhenNoFreeWrappedBox) {
    const int Dx = 4, Dy = 4, Dz = 4;
    std::vector<int> dims = {Dx, Dy, Dz};
    std::vector<int> free_npus = {
        npu_id(0, 0, 0, Dx, Dy),
        npu_id(2, 0, 0, Dx, Dy),
    };
    ClusterView view(free_npus, dims, Dx * Dy * Dz, 0);
    JobInstance job = make_job(/*job_id=*/7, /*num_ranks=*/3,
                               /*shape=*/{3, 1, 1});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    EXPECT_EQ(result.outcome, PlacementOutcome::DEFER);
    EXPECT_TRUE(result.npus.empty());
}

// ---------------------------------------------------------------------------
// Case 9: DROP when num_ranks > total_npus.
// ---------------------------------------------------------------------------
TEST(FirstFitBaseline, DropWhenNumRanksExceedsTotal) {
    const int Dx = 4, Dy = 4, Dz = 4;
    ClusterView view = make_full_cluster(Dx, Dy, Dz);
    // num_ranks deliberately larger than 64 = Dx*Dy*Dz.
    JobInstance job = make_job(/*job_id=*/2, /*num_ranks=*/100,
                               /*shape=*/{5, 5, 4});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    EXPECT_EQ(result.outcome, PlacementOutcome::DROP);
    EXPECT_TRUE(result.npus.empty());
}

// ---------------------------------------------------------------------------
// Case 10: DROP when shape exceeds cluster dims under every rotation.
// Shape (5, 1, 1) on a 4-wide cluster — no rotation fits, regardless of wrap.
// ---------------------------------------------------------------------------
TEST(FirstFitBaseline, DropWhenShapeExceedsDimsAllRotations) {
    const int Dx = 4, Dy = 4, Dz = 4;
    ClusterView view = make_full_cluster(Dx, Dy, Dz);
    JobInstance job = make_job(/*job_id=*/3, /*num_ranks=*/5,
                               /*shape=*/{5, 1, 1});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    EXPECT_EQ(result.outcome, PlacementOutcome::DROP);
    EXPECT_TRUE(result.npus.empty());
}

// ---------------------------------------------------------------------------
// Case 11: wrap enables a placement that pre-change would DEFER.
// Cluster 4x4x4. Free NPUs at (y=0,z=0) are x = 0, 1, 3. Pre-change,
// anchor x scans 0..1 (since 1+3<=4 but 2+3>4); both fail (x=2 busy).
// Post-change, anchor x=3 wraps: ranks x=3,0,1 — all free.
//
// Asserts:
//   (a) outcome PLACED
//   (b) every NPU in rank_map is in the constructed free set
//   (c) for some axis, consecutive ranks have axis coord D-1 -> 0
// ---------------------------------------------------------------------------
TEST(FirstFitWrap, WrapEnablesPlacementThatWouldOtherwiseDefer) {
    const int Dx = 4, Dy = 4, Dz = 4;
    std::vector<int> dims = {Dx, Dy, Dz};
    std::vector<int> free_npus = {
        npu_id(0, 0, 0, Dx, Dy),
        npu_id(1, 0, 0, Dx, Dy),
        npu_id(3, 0, 0, Dx, Dy),
    };
    ClusterView view(free_npus, dims, Dx * Dy * Dz, 0);
    JobInstance job = make_job(/*job_id=*/8, /*num_ranks=*/3,
                               /*shape=*/{3, 1, 1});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    // (a) PLACED.
    ASSERT_EQ(result.outcome, PlacementOutcome::PLACED);
    ASSERT_EQ(result.npus.size(), 3u);

    // (b) Every NPU is in the free set.
    std::vector<int> free_sorted = free_npus;
    std::sort(free_sorted.begin(), free_sorted.end());
    for (int n : result.npus) {
        EXPECT_TRUE(
            std::binary_search(free_sorted.begin(), free_sorted.end(), n))
            << "rank NPU " << n << " is not in the free set";
    }

    // (c) Seam crossing on some axis: consecutive ranks i, i+1 satisfy
    //     (coord_i == D-1) && (coord_{i+1} == 0) for that axis.
    auto coord_x = [&](int n) { return n % Dx; };
    auto coord_y = [&](int n) { return (n / Dx) % Dy; };
    auto coord_z = [&](int n) { return n / (Dx * Dy); };
    bool found_seam = false;
    for (size_t i = 0; i + 1 < result.npus.size(); ++i) {
        int a = result.npus[i];
        int b = result.npus[i + 1];
        if ((coord_x(a) == Dx - 1 && coord_x(b) == 0) ||
            (coord_y(a) == Dy - 1 && coord_y(b) == 0) ||
            (coord_z(a) == Dz - 1 && coord_z(b) == 0)) {
            found_seam = true;
            break;
        }
    }
    EXPECT_TRUE(found_seam)
        << "expected at least one seam crossing in rank_map";
}

// ---------------------------------------------------------------------------
// Case 12: rotation interplay with wrap.
// Shape (4, 2, 1) on a 4x4x4 cluster — shape[0] == D_x triggers the de-dup
// along the x axis under ax=(0,1,2). The empty cluster admits this trivially,
// so we additionally constrain that only y in {3, 0} is free at (z=0) for
// ranks along the y axis, forcing a y-wrap.
//
// Free NPUs: every x at (y=3, z=0) and every x at (y=0, z=0). 8 NPUs total.
// Algorithm tries ax=(0,1,2), placed_dims=(4,2,1). axis_upper(0)=1 (dedup),
// axis_upper(1)=Dy=4, axis_upper(2)=Dz=4. Anchor (axx=0, ay=3, az=0):
// ranks (x, 3, 0) for x=0..3 and (x, 0, 0) for x=0..3 (y wraps 3->0).
// ---------------------------------------------------------------------------
TEST(FirstFitWrap, RotationInterplayWithWrap) {
    const int Dx = 4, Dy = 4, Dz = 4;
    std::vector<int> dims = {Dx, Dy, Dz};
    std::vector<int> free_npus;
    for (int x = 0; x < Dx; ++x) {
        free_npus.push_back(npu_id(x, 3, 0, Dx, Dy));
        free_npus.push_back(npu_id(x, 0, 0, Dx, Dy));
    }
    ClusterView view(free_npus, dims, Dx * Dy * Dz, 0);
    JobInstance job = make_job(/*job_id=*/9, /*num_ranks=*/8,
                               /*shape=*/{4, 2, 1});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);

    ASSERT_EQ(result.outcome, PlacementOutcome::PLACED);
    ASSERT_EQ(result.npus.size(), 8u);

    // All result NPUs must be in the free set.
    std::vector<int> free_sorted = free_npus;
    std::sort(free_sorted.begin(), free_sorted.end());
    for (int n : result.npus) {
        EXPECT_TRUE(
            std::binary_search(free_sorted.begin(), free_sorted.end(), n))
            << "rank NPU " << n << " is not in the free set";
    }
}

// ---------------------------------------------------------------------------
// Case 13: DROP when permanently-failed NPUs make every anchor x rotation
// unsatisfiable forever. On 4x4x4 every {4,4,2} placement spans two full
// adjacent slabs along some axis; failing (0,0,0) and (2,2,2) puts a failed
// node in every such slab pair, so the shape can never place again even
// though its footprint fits the torus dims. Pre-fix this DEFERred to
// simulation exit and tripped the simulation_done() liveness assert.
// ---------------------------------------------------------------------------
TEST(FirstFitFailures, DegradedIdleUnplaceableShapeDrops) {
    const int Dx = 4, Dy = 4, Dz = 4;
    std::unordered_set<int> failed = {npu_id(0, 0, 0, Dx, Dy),
                                      npu_id(2, 2, 2, Dx, Dy)};
    std::vector<int> free_npus;
    for (int i = 0; i < Dx * Dy * Dz; ++i) {
        if (failed.find(i) == failed.end()) {
            free_npus.push_back(i);
        }
    }
    ClusterView view(std::move(free_npus), {Dx, Dy, Dz}, /*total=*/62,
                     /*current_tick=*/0, failed);
    JobInstance job = make_job(/*job_id=*/10, /*num_ranks=*/32,
                               /*shape=*/{4, 4, 2});

    FirstFit policy;
    EXPECT_EQ(policy.try_place(job, view).outcome, PlacementOutcome::DROP);
}

// ---------------------------------------------------------------------------
// Case 14: a shape that still fits the degraded torus keeps DEFERring while
// the cluster is merely busy.
// ---------------------------------------------------------------------------
TEST(FirstFitFailures, DegradedPlaceableShapeDefersWhenBusy) {
    const int Dx = 4, Dy = 4, Dz = 4;
    std::unordered_set<int> failed = {npu_id(0, 0, 0, Dx, Dy),
                                      npu_id(2, 2, 2, Dx, Dy)};
    // Nothing free right now.
    ClusterView view({}, {Dx, Dy, Dz}, /*total=*/62, /*current_tick=*/0,
                     failed);
    JobInstance job = make_job(/*job_id=*/11, /*num_ranks=*/8,
                               /*shape=*/{2, 2, 2});

    FirstFit policy;
    EXPECT_EQ(policy.try_place(job, view).outcome, PlacementOutcome::DEFER);
}

// ---------------------------------------------------------------------------
// Case 15: placement on the degraded torus dodges the failed NPUs.
// ---------------------------------------------------------------------------
TEST(FirstFitFailures, PlacementAvoidsFailedNpus) {
    const int Dx = 4, Dy = 4, Dz = 4;
    std::unordered_set<int> failed = {npu_id(0, 0, 0, Dx, Dy),
                                      npu_id(2, 2, 2, Dx, Dy)};
    std::vector<int> free_npus;
    for (int i = 0; i < Dx * Dy * Dz; ++i) {
        if (failed.find(i) == failed.end()) {
            free_npus.push_back(i);
        }
    }
    ClusterView view(std::move(free_npus), {Dx, Dy, Dz}, /*total=*/62,
                     /*current_tick=*/0, failed);
    JobInstance job = make_job(/*job_id=*/12, /*num_ranks=*/8,
                               /*shape=*/{2, 2, 2});

    FirstFit policy;
    PlacementResult result = policy.try_place(job, view);
    ASSERT_EQ(result.outcome, PlacementOutcome::PLACED);
    for (int n : result.npus) {
        EXPECT_EQ(failed.find(n), failed.end())
            << "rank NPU " << n << " is a failed node";
    }
}
