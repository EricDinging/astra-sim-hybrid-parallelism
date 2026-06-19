/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/BlockModel.hh"

#include <map>
#include <set>
#include <tuple>

namespace AstraSim {
namespace Scheduling {

BlockModel::BlockModel(std::array<int, 3> dims, std::array<int, 3> block)
    : dims_(dims),
      block_(block) {}

bool BlockModel::valid() const {
    for (int d = 0; d < 3; ++d) {
        if (block_[d] <= 0 || dims_[d] % block_[d] != 0) {
            return false;
        }
    }
    return true;
}

std::array<int, 3> BlockModel::coord(int id) const {
    int x = id % dims_[0];
    int y = (id / dims_[0]) % dims_[1];
    int z = id / (dims_[0] * dims_[1]);
    return {x, y, z};
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
    // The two nodes must sit on opposite faces of `axis` (+face <-> -face).
    // Only `axis` differs, so the perpendicular coords are equal and the
    // within-face position matches by construction.
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
