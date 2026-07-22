/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_TESTS_SCHEDULING_RDCN_CHECK_HH__
#define __ASTRASIM_TESTS_SCHEDULING_RDCN_CHECK_HH__

// RDCN-model legality checker (docs/rdcn-model.md, agreed 2026-07-21) for
// unit tests and micro-benchmarks. Verifies a PlacementResult honors the
// hardware constraints no optimization may relax:
//   R2 surface-only seams: every wired circuit joins face ports.
//   R3 legality: circuits pass the model's ocs_realizable_scatter predicate
//      (same axis + matching within-face position; polarity per model mode).
//   R5 port exclusivity: no port serves a re-pointed circuit and a
//      default-seam ride at once, and the wired set is directions-disjoint.
//   S3 shared fabric: multi-hop ring edges are permitted, but counted, so a
//      benchmark can bound them.
// Also classifies every logical ring edge for metric reporting.

#include "astra-sim/scheduling/BlockModel.hh"
#include "astra-sim/scheduling/Common.hh"

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace AstraSim {
namespace Scheduling {

struct RdcnReport {
    bool ok = true;
    std::string violation;  // first violation, empty when ok
    int intra_cube = 0;     // ring edges on intra-cube mesh links
    int default_seam = 0;   // ring edges on boot-configuration circuits
    int wired = 0;          // ring edges on re-pointed circuits
    int multihop = 0;       // ring edges left to shared-fabric routing (S3)
    // Cubes this placement occupies only partially (the damage metric).
    int frag_cubes = 0;
    int cubes_touched = 0;
};

inline RdcnReport rdcn_check(const PlacementResult& r,
                             const std::vector<std::pair<int, int>>& ring,
                             const BlockModel& cm,
                             std::array<int, 3> block,
                             std::array<int, 3> dims) {
    RdcnReport rep;
    auto fail = [&](const std::string& why) {
        if (rep.ok) {
            rep.ok = false;
            rep.violation = why;
        }
    };

    std::set<std::pair<int, int>> wired;
    for (const auto& e : r.reconfig_plan.ocs_edges) {
        wired.insert(
            {std::min(e.first, e.second), std::max(e.first, e.second)});
        if (!cm.ocs_realizable_scatter(e.first, e.second)) {
            std::ostringstream os;
            os << "R3 violation: wired circuit " << e.first << "<->" << e.second
               << " is not realizable";
            fail(os.str());
        }
    }
    if (!cm.directions_disjoint(r.reconfig_plan.ocs_edges)) {
        fail("R5 violation: wired circuits double-book a port");
    }

    // Port usage: (node, axis, +1/-1) -> re-pointed circuit or default seam.
    // A port serving both at once violates R5 (re-pointing a face port
    // unplugs its boot-configuration circuit).
    std::map<std::tuple<int, int, int>, char> port;  // 'w' wired, 'd' default
    auto face_dir = [&](int node, int ax) {
        return cm.is_face_node(node, ax, +1) ? +1 : -1;
    };
    auto seam_axis_dir = [&](int u, int v) -> std::pair<int, int> {
        // (axis, dir from u toward v) for a torus-adjacent pair.
        auto a = cm.coord(u);
        auto b = cm.coord(v);
        for (int ax = 0; ax < 3; ++ax) {
            if (a[ax] == b[ax]) {
                continue;
            }
            int fwd = (b[ax] - a[ax] + dims[ax]) % dims[ax];
            return {ax, fwd == 1 ? +1 : -1};
        }
        return {-1, 0};
    };
    auto claim = [&](int node, int ax, int dir, char kind) {
        if (block[ax] == 1) {
            return;  // 1-wide axis: both ports on every node; the
                     // directions_disjoint special case covers these.
        }
        auto key = std::make_tuple(node, ax, dir);
        auto it = port.find(key);
        if (it == port.end()) {
            port[key] = kind;
        } else if (it->second != kind) {
            std::ostringstream os;
            os << "R5 violation: node " << node << " axis " << ax << " dir "
               << dir << " serves both a wired circuit and a default seam";
            fail(os.str());
        }
    };
    for (const auto& e : r.reconfig_plan.ocs_edges) {
        int ax = -1;
        for (int d = 0; d < 3 && ax < 0; ++d) {
            auto a = cm.coord(e.first);
            auto b = cm.coord(e.second);
            int wu = a[d] % block[d];
            int wv = b[d] % block[d];
            bool uf = wu == 0 || wu == block[d] - 1;
            bool vf = wv == 0 || wv == block[d] - 1;
            bool perp = true;
            for (int p = 0; p < 3; ++p) {
                if (p != d && a[p] % block[p] != b[p] % block[p]) {
                    perp = false;
                }
            }
            if (uf && vf && perp) {
                ax = d;
            }
        }
        if (ax >= 0) {
            claim(e.first, ax, face_dir(e.first, ax), 'w');
            claim(e.second, ax, face_dir(e.second, ax), 'w');
        }
    }

    // Classify every logical ring edge.
    for (const auto& e : ring) {
        int u = r.npus[e.first];
        int v = r.npus[e.second];
        auto key = std::make_pair(std::min(u, v), std::max(u, v));
        if (wired.count(key) > 0) {
            ++rep.wired;
        } else if (cm.is_torus_adjacent(u, v)) {
            if (cm.same_block(u, v)) {
                ++rep.intra_cube;
            } else {
                ++rep.default_seam;
                auto [ax, dir] = seam_axis_dir(u, v);
                claim(u, ax, dir, 'd');
                claim(v, ax, -dir, 'd');
            }
        } else {
            ++rep.multihop;  // rides the shared fabric — S3 permits this
        }
    }

    // Dirty-cube accounting (the damage metric, not a legality rule).
    std::map<std::array<int, 3>, int> per_cube;
    for (int n : r.npus) {
        ++per_cube[cm.block_of(n)];
    }
    rep.cubes_touched = static_cast<int>(per_cube.size());
    const int cube_vol = block[0] * block[1] * block[2];
    for (const auto& [c, cnt] : per_cube) {
        if (cnt < cube_vol) {
            ++rep.frag_cubes;
        }
    }
    return rep;
}

}  // namespace Scheduling
}  // namespace AstraSim
#endif
