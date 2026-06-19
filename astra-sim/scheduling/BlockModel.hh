/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_BLOCKMODEL_HH__
#define __ASTRASIM_SCHEDULING_BLOCKMODEL_HH__

#include <array>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// Maps global NPU ids to block / within-block coordinates for a torus of `dims`
// tiled into blocks of per-axis dimensions `block` (may be non-cubic, e.g.
// 4x4x2). x-fastest: id = z*Dy*Dx + y*Dx + x.
class BlockModel {
  public:
    BlockModel(std::array<int, 3> dims, std::array<int, 3> block);
    bool valid() const;  // every block dim > 0 and divides its torus dim
    std::array<int, 3> coord(int id) const;  // (x, y, z)
    std::array<int, 3> block_of(
        int id) const;  // block coords = coord / block (per axis)
    bool same_block(int u, int v) const;
    // dir > 0 -> the +axis face (within-block coord == block[axis]-1);
    // dir <= 0 -> the -axis face (within-block coord == 0).
    bool is_face_node(int id, int axis, int dir) const;
    // True iff u and v are exactly one torus hop apart: their coords differ in
    // exactly one dimension, by 1 or by (dim-1) (the full-axis wrap). Tells
    // which ring edges already have a baseline torus link vs. need a new OCS
    // link.
    bool is_torus_adjacent(int u, int v) const;
    // True iff an OCS link between u and v is physically realizable: they
    // differ in exactly one dimension and sit on opposite faces of it (+face
    // <-> -face) at the same within-face position. Rejects placements whose OCS
    // edges would land on interior (non-face) nodes.
    bool ocs_realizable(int u, int v) const;
    // Global id from (x,y,z) coords (inverse of coord()).
    int id_of(std::array<int, 3> c) const;
    // True iff an OCS link u<->v is realizable when the two blocks may sit
    // ANYWHERE in the pod (the scatter case): u and v are on opposite faces of
    // one axis with matching within-block coords on the other two axes. Looser
    // than ocs_realizable, which also requires perpendicular block alignment.
    bool ocs_realizable_scatter(int u, int v) const;
    // Number of DISTINCT torus-adjacent blocks (block-grid neighbors, wrapped)
    // that contain at least one free NPU. Lower = more isolated. Used by the
    // max-defrag block ranking.
    int free_neighbor_blocks(std::array<int, 3> blk,
                             const std::unordered_set<int>& free) const;
    // True iff no node is the endpoint of two OCS edges on the same
    // (axis, +/-direction). Holds by construction for a valid embedding;
    // asserted as a safety net. Caller contract: pass only scatter-realizable
    // edges (RFold asserts ocs_realizable_scatter on each edge first). An edge
    // that is not a realizable face pair is ignored (not counted as a
    // conflict).
    bool directions_disjoint(
        const std::vector<std::pair<int, int>>& ocs_edges) const;
    std::array<int, 3> block_dims() const {
        return block_;
    }

  private:
    std::array<int, 3> dims_;
    std::array<int, 3> block_;
    // The single axis along which u,v form a realizable OCS face pair, or -1.
    int ocs_axis(int u, int v) const;
};

}  // namespace Scheduling
}  // namespace AstraSim
#endif
