/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/FoldEnumerator.hh"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>

#include "astra-sim/common/Logging.hh"

namespace AstraSim {
namespace Scheduling {

std::vector<int> snake_coords(int v, const std::vector<int>& sizes) {
    const int m = static_cast<int>(sizes.size());
    std::vector<int> digit(m, 0);
    std::vector<int> higher(m, 0);  // digits above k, as a number
    int rem = v;
    for (int k = 0; k < m; ++k) {
        digit[k] = rem % sizes[k];
        rem /= sizes[k];
        higher[k] = rem;
    }
    std::vector<int> coord(m, 0);
    for (int k = 0; k < m; ++k) {
        coord[k] = (higher[k] % 2 == 1) ? (sizes[k] - 1 - digit[k]) : digit[k];
    }
    return coord;
}

int snake_end_gap(const std::vector<int>& sizes) {
    int n = 1;
    for (int s : sizes) {
        n *= s;
    }
    auto end = snake_coords(n - 1, sizes);
    int gap = 0;
    for (int g : end) {
        gap += g;
    }
    return gap;
}

std::vector<std::vector<int>> ordered_factorizations(int n, int max_parts) {
    std::vector<std::vector<int>> out;
    std::vector<int> cur;
    std::function<void(int)> rec = [&](int rem) {
        if (rem == 1) {
            if (!cur.empty()) {
                out.push_back(cur);
            }
            return;
        }
        if (static_cast<int>(cur.size()) == max_parts) {
            return;
        }
        for (int f = 2; f <= rem; ++f) {
            if (rem % f != 0) {
                continue;
            }
            cur.push_back(f);
            rec(rem / f);
            cur.pop_back();
        }
    };
    rec(n);
    return out;
}

std::vector<int> FoldEnumerator::snake_1d(const std::vector<int>& free,
                                          const std::vector<int>& dims,
                                          int len,
                                          int max_depth) {
    if (len <= 0 || static_cast<int>(free.size()) < len) {
        return {};
    }
    const int Dx = dims[0], Dy = dims[1], Dz = dims[2];
    std::unordered_set<int> free_set(free.begin(), free.end());

    auto neighbors = [&](int n) {
        std::vector<int> nb;
        int x = n % Dx, y = (n / Dx) % Dy, z = n / (Dx * Dy);
        auto add = [&](int nx, int ny, int nz) {
            int id = nz * Dy * Dx + ny * Dx + nx;
            if (free_set.count(id)) {
                nb.push_back(id);
            }
        };
        if (Dx > 1) {
            add((x + 1) % Dx, y, z);
            if (Dx > 2) {
                add((x - 1 + Dx) % Dx, y, z);
            }
        }
        if (Dy > 1) {
            add(x, (y + 1) % Dy, z);
            if (Dy > 2) {
                add(x, (y - 1 + Dy) % Dy, z);
            }
        }
        if (Dz > 1) {
            add(x, y, (z + 1) % Dz);
            if (Dz > 2) {
                add(x, y, (z - 1 + Dz) % Dz);
            }
        }
        return nb;
    };
    auto adjacent = [&](int a, int b) {
        int ax = a % Dx, ay = (a / Dx) % Dy, az = a / (Dx * Dy);
        int bx = b % Dx, by = (b / Dx) % Dy, bz = b / (Dx * Dy);
        auto d1 = [&](int u, int w, int D) {
            int e = std::abs(u - w);
            return std::min(e, D - e);
        };
        return d1(ax, bx, Dx) + d1(ay, by, Dy) + d1(az, bz, Dz) == 1;
    };

    int budget = max_depth;
    std::vector<int> path;
    std::unordered_set<int> on_path;
    std::function<bool(int)> dfs = [&](int start) -> bool {
        if (static_cast<int>(path.size()) == len) {
            return adjacent(path.back(), start);
        }
        if (budget-- <= 0) {
            return false;
        }
        for (int nb : neighbors(path.back())) {
            if (on_path.count(nb)) {
                continue;
            }
            path.push_back(nb);
            on_path.insert(nb);
            if (dfs(start)) {
                return true;
            }
            on_path.erase(nb);
            path.pop_back();
        }
        return false;
    };

    for (int s : free) {
        path = {s};
        on_path = {s};
        if (dfs(s)) {
            return path;
        }
        if (budget <= 0) {
            break;
        }
    }
    return {};
}

namespace {

// A fold "plan" describes how the K logical ranks map onto a 3-D footprint.
// Each logical dimension may be split across several footprint axes (folding),
// and an axis may carry segments from several dimensions (a joint fold donates
// a factor of 2 onto an axis already owned by another dimension).
//
//  - dim_axes[dim]   : footprint axes this dim occupies, inner -> outer
//  - dim_factor[dim] : the dim's factor on each of those axes (same order)
//  - axis_segs[axis] : (dim, factor) segments living on this axis, inner->outer
//  - size[axis]      : product of the axis's segment factors (== footprint dim)
struct Plan {
    std::array<int, 3> size;
    std::array<std::vector<int>, 3> dim_axes;
    std::array<std::vector<int>, 3> dim_factor;
    std::array<std::vector<std::pair<int, int>>, 3> axis_segs;
};

std::array<int, 3> decode_rank(int i, const std::array<int, 3>& s) {
    return {i % s[0], (i / s[0]) % s[1], i / (s[0] * s[1])};
}

// A folded ring closes to a 1-hop cycle only when its boustrophedon end-gap is
// 1 AND its outer axis is not shared with another dimension (sharing makes the
// outer flip a multi-step move on a non-full torus axis). The end-gap is 1
// exactly when the serpentine over the dim's factor tuple returns to within one
// hop of the start (for a 2-level tuple this is "outer factor == 2"; the same
// end-gap rule generalizes to arbitrary depth without changing which tuples
// close structurally — closing by full-axis OCS wrap at placement time is a
// separate mechanism). Single-axis (unfolded) rings are neutral -- their
// closure is a placement-time full-axis property enumeration can't know. So
// joint 3-D folds, whose donor shares its outer axis, are conservatively
// reported as open; they earn their keep through defragmentation, not comm.
bool dim_closes(const Plan& p, int dim, const std::array<int, 3>& shape) {
    if (shape[dim] <= 1) {
        return true;  // trivial: no ring
    }
    const auto& axes = p.dim_axes[dim];
    if (axes.size() < 2) {
        return true;  // single-axis line: neutral
    }
    int outer_axis = axes.back();
    bool sole_on_outer = p.axis_segs[outer_axis].size() == 1;
    return snake_end_gap(p.dim_factor[dim]) == 1 && sole_on_outer;
}

FoldVariant build_variant(const Plan& p, const std::array<int, 3>& shape) {
    FoldVariant v;
    v.footprint = p.size;
    int K = shape[0] * shape[1] * shape[2];
    v.embedding.resize(K);
    for (int i = 0; i < K; ++i) {
        auto dv = decode_rank(i, shape);
        // digit[axis][dim] = this dim's boustrophedon digit on that axis.
        std::array<std::array<int, 3>, 3> digit{};
        for (int dim = 0; dim < 3; ++dim) {
            if (p.dim_axes[dim].empty()) {
                continue;
            }
            auto sc = snake_coords(dv[dim], p.dim_factor[dim]);
            for (size_t k = 0; k < p.dim_axes[dim].size(); ++k) {
                digit[p.dim_axes[dim][k]][dim] = sc[k];
            }
        }
        std::array<int, 3> coord{0, 0, 0};
        for (int ax = 0; ax < 3; ++ax) {
            int stride = 1;
            for (const auto& seg : p.axis_segs[ax]) {  // inner -> outer
                coord[ax] += digit[ax][seg.first] * stride;
                stride *= seg.second;
            }
        }
        v.embedding[i] = coord;
    }
    v.ring_closes = dim_closes(p, 0, shape) && dim_closes(p, 1, shape) &&
                    dim_closes(p, 2, shape);
    return v;
}

Plan identity_plan(const std::array<int, 3>& shape) {
    Plan p;
    for (int a = 0; a < 3; ++a) {
        p.size[a] = shape[a];
        if (shape[a] > 1) {
            p.dim_axes[a] = {a};
            p.dim_factor[a] = {shape[a]};
            p.axis_segs[a] = {{a, shape[a]}};
        }
    }
    return p;
}

// Comm class of a plan for footprint-collision arbitration (the identity is
// handled separately in enumerate and always outranks both): 0 = every folded
// ring closes (hairpins), 1 = some folded ring left open (joint folds).
int plan_class(const Plan& p, const std::array<int, 3>& shape) {
    return (dim_closes(p, 0, shape) && dim_closes(p, 1, shape) &&
            dim_closes(p, 2, shape))
               ? 0
               : 1;
}

// One bisection step: a donor dim d (occupying a single, unshared axis a with
// an even factor) gives a factor of 2 to a target axis t. t may be a spare axis
// (size 1) or an axis owned by a single dim with an EVEN factor (a joint fold).
// The even-partner rule is exactly why 4x8x3 -> 4x4x6 is rejected (PP=3 is
// odd). Recurses; keeps the best PLAN per footprint: distinct paths CAN reach
// the same footprint with different comm quality -- e.g. for (1,8,8) the DFS
// finds 2x8x4 first as an open joint fold (inside the 2x4x8 hairpin's
// recursion) and only later as the direct PP hairpin. Same footprint means
// identical placeability and within-class cost, so a ring-closing plan
// strictly dominates an open one; equal classes keep the first (DFS) plan.
void grow(const Plan& p,
          std::vector<Plan>& out,
          std::map<std::array<int, 3>, size_t>& best,
          const std::array<int, 3>& id_size,
          const std::array<int, 3>& shape) {
    for (int d = 0; d < 3; ++d) {
        if (p.dim_axes[d].size() != 1) {
            continue;  // donor must occupy exactly one axis
        }
        int a = p.dim_axes[d][0];
        if (p.axis_segs[a].size() != 1) {
            continue;  // donor's axis must be unshared
        }
        int fa = p.dim_factor[d][0];
        if (fa % 2 != 0 || fa < 4) {
            continue;  // keep >= 2 on axis a after the split
        }
        for (int t = 0; t < 3; ++t) {
            if (t == a) {
                continue;
            }
            bool spare = p.size[t] == 1;
            bool joint = !spare && p.axis_segs[t].size() == 1 &&
                         p.axis_segs[t][0].second % 2 == 0;
            if (!spare && !joint) {
                continue;
            }
            Plan q = p;
            q.dim_factor[d][0] = fa / 2;
            q.size[a] = fa / 2;
            q.axis_segs[a] = {{d, fa / 2}};
            q.dim_axes[d].push_back(t);
            q.dim_factor[d].push_back(2);
            q.size[t] = p.size[t] * 2;         // spare: 1*2; joint: s_partner*2
            q.axis_segs[t].push_back({d, 2});  // donor's "2" goes outer
            if (q.size == id_size) {
                continue;  // the identity variant always wins its footprint
            }
            auto it = best.find(q.size);
            if (it == best.end()) {
                best.emplace(q.size, out.size());
                out.push_back(q);
                grow(q, out, best, id_size, shape);
            } else if (plan_class(q, shape) <
                       plan_class(out[it->second], shape)) {
                // Terminates: a footprint slot improves at most once (1 -> 0).
                out[it->second] = q;
                grow(q, out, best, id_size, shape);
            }
        }
    }
}

// Exclusive multi-factor plans (multi-folds): every dimension's ordered
// factor tuple occupies its own axes — the conservative spare-axes rule.
// Axis-sharing (joint folds) remains grow()'s job; overlapping footprints
// are arbitrated by the shared best-map — equal comm class keeps grow's
// incumbent plan, but a strictly better class REPLACES it (e.g. for
// {4,4,2}, the open joint fold at footprint 2x4x4 is displaced by the
// closed rotated-identity exclusive plan — same footprint, same
// placeability, strictly better comm class). Within a dim, factors map to
// axes in increasing axis order: permuted assignments are placement
// rotations of each other and the contiguous scan already sweeps rotations.
void enumerate_exclusive(const std::array<int, 3>& shape,
                         std::vector<Plan>& out,
                         std::map<std::array<int, 3>, size_t>& best,
                         const std::array<int, 3>& id_size) {
    std::array<std::vector<std::vector<int>>, 3> tuples;
    for (int d = 0; d < 3; ++d) {
        if (shape[d] > 1) {
            tuples[d] = ordered_factorizations(shape[d], 3);
        } else {
            tuples[d] = {{}};  // size-1 dim occupies no axis
        }
    }
    std::function<void(int, const Plan&, unsigned)> rec =
        [&](int d, const Plan& p, unsigned used) {
            if (d == 3) {
                if (p.size == id_size) {
                    return;  // identity always wins its own footprint
                }
                auto it = best.find(p.size);
                if (it == best.end()) {
                    best.emplace(p.size, out.size());
                    out.push_back(p);
                } else if (plan_class(p, shape) <
                           plan_class(out[it->second], shape)) {
                    out[it->second] = p;
                }
                return;
            }
            for (const auto& t : tuples[d]) {
                std::function<void(size_t, const Plan&, unsigned)> assign =
                    [&](size_t k, const Plan& q, unsigned u) {
                        if (k == t.size()) {
                            rec(d + 1, q, u);
                            return;
                        }
                        for (int ax = 0; ax < 3; ++ax) {
                            if (u & (1u << ax)) {
                                continue;
                            }
                            if (!q.dim_axes[d].empty() &&
                                ax < q.dim_axes[d].back()) {
                                continue;  // canonical within-dim axis order
                            }
                            Plan r = q;
                            r.dim_axes[d].push_back(ax);
                            r.dim_factor[d].push_back(t[k]);
                            r.size[ax] = t[k];
                            r.axis_segs[ax] = {{d, t[k]}};
                            assign(k + 1, r, u | (1u << ax));
                        }
                    };
                Plan q = p;
                assign(0, q, used);
            }
        };
    Plan base;
    base.size = {1, 1, 1};
    rec(0, base, 0u);
}

}  // namespace

std::vector<FoldVariant> FoldEnumerator::enumerate(
    const std::array<int, 3>& shape, bool multifold) {
    std::vector<FoldVariant> variants;
    Plan id = identity_plan(shape);
    variants.push_back(build_variant(id, shape));  // identity first
    variants.front().identity = true;

    std::vector<Plan> plans;
    std::map<std::array<int, 3>, size_t> best;
    grow(id, plans, best, id.size, shape);
    if (multifold) {
        enumerate_exclusive(shape, plans, best, id.size);
    }
    for (const auto& p : plans) {
        variants.push_back(build_variant(p, shape));
    }
    constexpr size_t kVariantCap = 512;
    if (variants.size() > kVariantCap) {
        LoggerFactory::get_logger("scheduling")
            ->warn("FoldEnumerator: {} variants for {}x{}x{}; capping at {}",
                   variants.size(), shape[0], shape[1], shape[2], kVariantCap);
        variants.resize(kVariantCap);
    }
    return variants;
}

}  // namespace Scheduling
}  // namespace AstraSim
