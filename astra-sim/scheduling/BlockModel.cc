/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/BlockModel.hh"

#include "astra-sim/scheduling/Common.hh"

#include <map>
#include <set>
#include <tuple>

namespace AstraSim {
namespace Scheduling {

BlockModel::BlockModel(std::array<int, 3> dims,
                       std::array<int, 3> block,
                       bool polarity_free)
    : dims_(dims),
      block_(block),
      polarity_free_(polarity_free) {}

bool BlockModel::valid() const {
    for (int d = 0; d < 3; ++d) {
        if (block_[d] <= 0 || dims_[d] % block_[d] != 0) {
            return false;
        }
    }
    return true;
}

std::array<int, 3> BlockModel::coord(int id) const {
    return coord_of(id, dims_[0], dims_[1]);
}

std::array<int, 3> BlockModel::block_of(int id) const {
    auto c = coord(id);
    return {c[0] / block_[0], c[1] / block_[1], c[2] / block_[2]};
}

bool BlockModel::same_block(int u, int v) const {
    return block_of(u) == block_of(v);
}

bool BlockModel::is_face_node(int id, int axis, int dir) const {
    int w = coord(id)[axis] % block_[axis];
    return dir > 0 ? (w == block_[axis] - 1) : (w == 0);
}

bool BlockModel::is_torus_adjacent(int u, int v) const {
    if (u == v) {
        return false;
    }
    auto a = coord(u);
    auto b = coord(v);
    int diffs = 0;
    for (int d = 0; d < 3; ++d) {
        int delta = a[d] > b[d] ? a[d] - b[d] : b[d] - a[d];
        if (delta == 0) {
            continue;
        }
        if (delta == 1 || delta == dims_[d] - 1) {
            ++diffs;
        } else {
            return false;  // a jump >1 (and not a full-axis wrap) -> not
                           // adjacent
        }
    }
    return diffs == 1;
}

bool BlockModel::ocs_realizable(int u, int v) const {
    if (u == v) {
        return false;
    }
    auto a = coord(u);
    auto b = coord(v);
    int axis = -1;
    for (int d = 0; d < 3; ++d) {
        if (a[d] != b[d]) {
            if (axis != -1) {
                return false;  // differs in >1 dim -> not a single OCS link
            }
            axis = d;
        }
    }
    if (axis == -1) {
        return false;
    }
    // Only `axis` differs, so the perpendicular coords are equal and the
    // within-face position matches by construction. Strict mode: the nodes
    // must sit on opposite faces of `axis` (+face <-> -face). Polarity-free
    // (corrected R3): any two face ports of `axis` pair up; a same-block
    // same-face pair would be the same node, which the u == v guard excludes.
    const bool u_face = is_face_node(u, axis, +1) || is_face_node(u, axis, -1);
    const bool v_face = is_face_node(v, axis, +1) || is_face_node(v, axis, -1);
    if (polarity_free_) {
        return u_face && v_face;
    }
    return (is_face_node(u, axis, +1) && is_face_node(v, axis, -1)) ||
           (is_face_node(u, axis, -1) && is_face_node(v, axis, +1));
}

int BlockModel::id_of(std::array<int, 3> c) const {
    return c[2] * dims_[0] * dims_[1] + c[1] * dims_[0] + c[0];
}

int BlockModel::ocs_axis(int u, int v) const {
    if (u == v) {
        return -1;
    }
    auto a = coord(u);
    auto b = coord(v);
    for (int ax = 0; ax < 3; ++ax) {
        int wu = a[ax] % block_[ax];
        int wv = b[ax] % block_[ax];
        bool opp = (wu == block_[ax] - 1 && wv == 0) ||
                   (wu == 0 && wv == block_[ax] - 1);
        if (polarity_free_) {
            // Corrected R3: cross-block circuits may join any two face ports
            // at matching position, same polarity included (+x<->+x). A
            // same-block pair with matching perpendicular coords and u != v
            // is opposite faces by construction, so no extra check needed.
            opp = (wu == 0 || wu == block_[ax] - 1) &&
                  (wv == 0 || wv == block_[ax] - 1);
        }
        if (!opp) {
            continue;
        }
        bool ok = true;
        for (int p = 0; p < 3; ++p) {
            if (p != ax && a[p] % block_[p] != b[p] % block_[p]) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return ax;
        }
    }
    return -1;
}

bool BlockModel::ocs_realizable_scatter(int u, int v) const {
    return ocs_axis(u, v) >= 0;
}

int BlockModel::free_neighbor_blocks(
    std::array<int, 3> blk, const std::unordered_set<int>& free) const {
    std::array<int, 3> g{dims_[0] / block_[0], dims_[1] / block_[1],
                         dims_[2] / block_[2]};
    std::set<std::array<int, 3>> neigh;
    for (int a = 0; a < 3; ++a) {
        for (int s = -1; s <= 1; s += 2) {
            std::array<int, 3> nb = blk;
            nb[a] = (blk[a] + s + g[a]) % g[a];
            if (nb != blk) {
                neigh.insert(nb);
            }
        }
    }
    int cnt = 0;
    for (const auto& nb : neigh) {
        bool has_free = false;
        for (int z = nb[2] * block_[2];
             z < nb[2] * block_[2] + block_[2] && !has_free; ++z) {
            for (int y = nb[1] * block_[1];
                 y < nb[1] * block_[1] + block_[1] && !has_free; ++y) {
                for (int x = nb[0] * block_[0];
                     x < nb[0] * block_[0] + block_[0] && !has_free; ++x) {
                    if (free.count(id_of({x, y, z})) > 0) {
                        has_free = true;
                    }
                }
            }
        }
        if (has_free) {
            ++cnt;
        }
    }
    return cnt;
}

std::vector<std::pair<int, int>> BlockModel::wirable_subset(
    const std::vector<std::pair<int, int>>& phys_ring_edges,
    const std::vector<std::pair<int, int>>& wires) const {
    if (!polarity_free_) {
        return wires;  // v1 model: torus links are hardware, no port sharing
    }
    // (node, axis, +1/-1) -> claimed. For 1-wide block axes every node holds
    // both ports of the axis; mirror directions_disjoint's allowance with a
    // per-(node, axis) degree cap of 2 instead of per-direction claims.
    std::set<std::tuple<int, int, int>> used;
    std::map<std::pair<int, int>, int> degree;
    auto claim = [&](int node, int ax, int dir) -> bool {
        if (block_[ax] == 1) {
            return ++degree[{node, ax}] <= 2;
        }
        return used.insert({node, ax, dir}).second;
    };
    std::set<std::pair<int, int>> seams;  // dedup: rings list both directions
    for (const auto& e : phys_ring_edges) {
        if (!is_torus_adjacent(e.first, e.second) ||
            same_block(e.first, e.second)) {
            continue;  // wires handled below; intra-block edges use the mesh
        }
        seams.insert(
            {std::min(e.first, e.second), std::max(e.first, e.second)});
    }
    for (const auto& e : seams) {
        auto a = coord(e.first);
        auto b = coord(e.second);
        for (int ax = 0; ax < 3; ++ax) {
            if (a[ax] == b[ax]) {
                continue;
            }
            int fwd = (b[ax] - a[ax] + dims_[ax]) % dims_[ax];
            int dir = fwd == 1 ? +1 : -1;
            claim(e.first, ax, dir);
            claim(e.second, ax, -dir);
            break;
        }
    }
    std::vector<std::pair<int, int>> kept;
    for (const auto& w : wires) {
        int ax = ocs_axis(w.first, w.second);
        if (ax < 0) {
            continue;  // not realizable at all; caller's predicate handles it
        }
        auto dir_of = [&](int node) {
            return is_face_node(node, ax, +1) ? +1 : -1;
        };
        // Check both endpoints, then commit both — a dropped wire must not
        // leave a half-claimed port behind. Endpoints are distinct nodes, so
        // the two checks never alias.
        const int du = dir_of(w.first);
        const int dv = dir_of(w.second);
        const bool ok =
            block_[ax] == 1
                ? degree[{w.first, ax}] < 2 && degree[{w.second, ax}] < 2
                : used.count({w.first, ax, du}) == 0 &&
                      used.count({w.second, ax, dv}) == 0;
        if (ok) {
            claim(w.first, ax, du);
            claim(w.second, ax, dv);
            kept.push_back(w);
        }
    }
    return kept;
}

bool BlockModel::directions_disjoint(
    const std::vector<std::pair<int, int>>& ocs_edges) const {
    if (block_[0] == 1 && block_[1] == 1 && block_[2] == 1) {
        // Unit blocks: every node is both faces of every axis, so any edge is
        // a face pair on ANY axis (ocs_axis collapses them all onto axis 0)
        // and each node owns all 6 OCS ports. A plan is realizable iff no
        // node carries more than 6 edges: a multigraph of max degree 6
        // splits into 3 max-degree-2 subgraphs (one per axis OCS group),
        // each a union of paths/cycles directable with one +/- port per
        // node and direction.
        std::map<int, int> degree;
        for (const auto& e : ocs_edges) {
            if (ocs_axis(e.first, e.second) < 0) {
                continue;
            }
            if (++degree[e.first] > 6 || ++degree[e.second] > 6) {
                return false;
            }
        }
        return true;
    }
    std::set<std::tuple<int, int, int>> used;
    std::map<std::pair<int, int>, int> degree;
    for (const auto& e : ocs_edges) {
        int ax = ocs_axis(e.first, e.second);
        if (ax < 0) {
            continue;
        }
        if (block_[ax] == 1) {
            // 1-wide axis of a non-unit block: both faces of this axis live
            // on every node, so it owns the + AND - port and an edge may use
            // either side. Up to two same-axis edges form paths/cycles that
            // can always be directed consistently; a third has no port pair
            // left. (Conservative for mixed blocks: an edge is charged to
            // its first realizable axis only.)
            for (int node : {e.first, e.second}) {
                if (++degree[{node, ax}] > 2) {
                    return false;
                }
            }
            continue;
        }
        for (int node : {e.first, e.second}) {
            int dir = is_face_node(node, ax, +1) ? +1 : -1;
            if (!used.insert({node, ax, dir}).second) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace Scheduling
}  // namespace AstraSim
