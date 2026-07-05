/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/FirstFit.hh"

#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_set>
#include <vector>

// FirstFit placement on a 3-D torus.
//
// This policy hard-assumes the cluster is a torus on every axis. Under DOR
// routing with bidi=false (the only mode this frontend uses today), every
// axis is a unidirectional ring with seam links populated by the BW matrix.
// The anchor scan therefore sweeps the full [0, D_i) range on each axis
// and indexes resulting coordinates modulo D_i, allowing placements to
// straddle the seam (e.g. x = D_x - 1, 0, 1, ...).
//
// When shape[i] == D_i along some axis, every anchor on that axis maps to
// the same physical sub-cube (modulo D_i). The de-dup rule pins the anchor
// to 0 on such axes to avoid wasted iterations.
//
// If mesh support is reintroduced later, ClusterView must learn an
// is_torus flag and the anchor/modulo logic below must branch on it.

namespace AstraSim {
namespace Scheduling {

namespace {

// id = z*D1*D0 + y*D0 + x
inline int coord_to_id(int x, int y, int z, const std::vector<int>& d) {
    return z * d[1] * d[0] + y * d[0] + x;
}

// Generate all 6 permutations of (DP, TP, PP).
std::vector<std::array<int, 3>> permutations(const std::array<int, 3>& s) {
    std::vector<std::array<int, 3>> out;
    std::array<int, 3> idx{0, 1, 2};
    do {
        out.push_back({s[idx[0]], s[idx[1]], s[idx[2]]});
    } while (std::next_permutation(idx.begin(), idx.end()));
    return out;
}

// True iff there exists any permutation of `shape` that is coord-wise <= dims.
bool any_perm_fits(const std::array<int, 3>& shape,
                   const std::vector<int>& dims) {
    for (const auto& p : permutations(shape)) {
        if (p[0] <= dims[0] && p[1] <= dims[1] && p[2] <= dims[2]) {
            return true;
        }
    }
    return false;
}

// DP fastest, PP slowest:
//   i = pp*TP*DP + tp*DP + dp
//   dp = i % DP; tp = (i/DP) % TP; pp = i/(DP*TP)
std::array<int, 3> decode_rank(int i, const std::array<int, 3>& shape) {
    int dp = i % shape[0];
    int tp = (i / shape[0]) % shape[1];
    int pp = i / (shape[0] * shape[1]);
    return {dp, tp, pp};
}

// `ax` is a permutation of (0,1,2) saying which cluster axis each job dim
// (DP/TP/PP) is assigned to. Map a job rank (dp, tp, pp) -> global NPU id.
int rank_to_npu(int dp,
                int tp,
                int pp,
                const std::array<int, 3>& anchor,
                const std::array<int, 3>& ax,
                const std::vector<int>& cluster_dims) {
    int per_axis[3] = {0, 0, 0};
    int job_coords[3] = {dp, tp, pp};
    for (int j = 0; j < 3; ++j) {
        per_axis[ax[j]] = job_coords[j];
    }
    // Modulo per axis to support wrap-around placements on the torus.
    // For non-wrapping anchors (anchor + offset < D_i) this is a no-op.
    int x = (anchor[0] + per_axis[0]) % cluster_dims[0];
    int y = (anchor[1] + per_axis[1]) % cluster_dims[1];
    int z = (anchor[2] + per_axis[2]) % cluster_dims[2];
    return coord_to_id(x, y, z, cluster_dims);
}

// Rotation x anchor scan: the first all-free cuboid embedding of `job`, or
// nullopt. Shared by placement (the real free set) and the idle-cluster
// oracle (every usable NPU free).
std::optional<std::vector<int>> scan_anchors(
    const JobInstance& job,
    const std::vector<int>& dims,
    const std::unordered_set<int>& free_set) {
    std::array<int, 3> ax{0, 1, 2};
    // One reusable candidate buffer across all anchors/rotations: most anchors
    // fail, so avoid a per-anchor allocation and stop building ids as soon as
    // one lands on an occupied node (mirrors ContiguousScan). Scan order and
    // the returned candidate are unchanged.
    std::vector<int> candidate;
    candidate.reserve(job.num_ranks);
    do {
        std::array<int, 3> placed_dims{0, 0, 0};
        for (int j = 0; j < 3; ++j) {
            placed_dims[ax[j]] = job.shape[j];
        }
        if (placed_dims[0] > dims[0] || placed_dims[1] > dims[1] ||
            placed_dims[2] > dims[2]) {
            continue;
        }
        // Anchor scan covers the full [0, D_i) range on each axis to allow
        // wrap-around placements. When the placed shape fully occupies an
        // axis, every anchor on that axis maps to the same physical sub-cube
        // modulo D_i; de-dup by pinning the anchor to 0.
        auto axis_upper = [&](int i) {
            return (placed_dims[i] == dims[i]) ? 1 : dims[i];
        };
        for (int az = 0; az < axis_upper(2); ++az) {
            for (int ay = 0; ay < axis_upper(1); ++ay) {
                for (int axx = 0; axx < axis_upper(0); ++axx) {
                    candidate.clear();
                    bool all_free = true;
                    for (int i = 0; i < job.num_ranks; ++i) {
                        auto rc = decode_rank(i, job.shape);
                        int npu = rank_to_npu(rc[0], rc[1], rc[2],
                                              {axx, ay, az}, ax, dims);
                        if (free_set.find(npu) == free_set.end()) {
                            all_free = false;
                            break;
                        }
                        candidate.push_back(npu);
                    }
                    if (all_free) {
                        return candidate;
                    }
                }
            }
        }
    } while (std::next_permutation(ax.begin(), ax.end()));
    return std::nullopt;
}

}  // namespace

PlacementResult FirstFit::try_place(const JobInstance& job,
                                    const ClusterView& view) {
    PlacementResult r;
    const auto& dims = view.physical_dims();
    if (dims.size() != 3) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "FirstFit requires 3-D physical_dims (--npus-per-dim)";
        return r;
    }

    if (!any_perm_fits(job.shape, dims) || job.num_ranks > view.total_npus()) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "shape cannot fit cluster under any rotation";
        return r;
    }

    std::unordered_set<int> free_set(view.free_npus().begin(),
                                     view.free_npus().end());
    if (auto found = scan_anchors(job, dims, free_set)) {
        r.outcome = PlacementOutcome::PLACED;
        r.npus = std::move(*found);
        return r;
    }

    // With permanently-failed NPUs, a shape that fits the torus dims may
    // still never dodge the failed nodes: every anchor x rotation stays
    // unsatisfiable forever, and the job would DEFER to simulation exit.
    // "Idle" therefore means the degraded torus, not the pristine one.
    if (!view.failed_npus().empty() && !placeable_on_idle(job, view)) {
        r.outcome = PlacementOutcome::DROP;
        r.reason = "no anchor + rotation fits the degraded cluster, even idle";
        return r;
    }

    r.outcome = PlacementOutcome::DEFER;
    r.reason = "no anchor + rotation fits in free NPUs right now";
    return r;
}

bool FirstFit::placeable_on_idle(const JobInstance& job,
                                 const ClusterView& view) {
    auto it = idle_ok_.find(job.shape);
    if (it != idle_ok_.end()) {
        return it->second;
    }
    // Idle = every usable NPU free; failed ids stay excluded for the whole
    // run, so a job unplaceable here is unplaceable forever.
    const auto& dims = view.physical_dims();
    const auto& failed = view.failed_npus();
    const int n = dims[0] * dims[1] * dims[2];
    std::unordered_set<int> idle_free;
    idle_free.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (failed.find(i) == failed.end()) {
            idle_free.insert(i);
        }
    }
    const bool ok = scan_anchors(job, dims, idle_free).has_value();
    idle_ok_.emplace(job.shape, ok);
    return ok;
}

}  // namespace Scheduling
}  // namespace AstraSim
