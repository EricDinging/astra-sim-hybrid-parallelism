/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/ScatterAssigner.hh"

#include "astra-sim/scheduling/BlockTiler.hh"
#include "astra-sim/scheduling/FootprintRouter.hh"

#include <algorithm>
#include <map>
#include <set>

namespace AstraSim {
namespace Scheduling {

namespace {

// Build rank_map from a complete tiled-block->physical-block assignment +
// offset vector.
std::vector<int> build_rank_map(
    const FoldVariant& v,
    const Tiling& t,
    const std::array<int, 3>& O,
    const std::vector<int>& dims,
    std::array<int, 3> blk,
    const std::map<std::array<int, 3>, std::array<int, 3>>& grid2blk) {
    std::vector<int> rm(v.embedding.size());
    for (size_t r = 0; r < v.embedding.size(); ++r) {
        const auto& e = v.embedding[r];
        std::array<int, 3> g{e[0] / blk[0], e[1] / blk[1], e[2] / blk[2]};
        const std::array<int, 3>& C = grid2blk.at(g);
        std::array<int, 3> gc;
        for (int i = 0; i < 3; ++i) {
            int wc = t.offset_free[i] ? (e[i] + O[i]) : (e[i] % blk[i]);
            gc[i] = C[i] * blk[i] + wc;
        }
        rm[r] = gc[2] * dims[0] * dims[1] + gc[1] * dims[0] + gc[0];
    }
    return rm;
}

// 1-D snake fast path shared by both assigners: a single ring longer than a
// block edge -> try a hardwired-adjacent cycle (0 OCS). Empty when the shape
// is not 1-D-oversized or no cycle exists within budget.
std::vector<int> snake_fast_path(const std::array<int, 3>& fp,
                                 const std::array<int, 3>& blk,
                                 const std::unordered_set<int>& free,
                                 const std::vector<int>& dims,
                                 int budget) {
    int nonunit = 0;
    int axis = -1;
    for (int i = 0; i < 3; ++i) {
        if (fp[i] > 1) {
            ++nonunit;
            axis = i;
        }
    }
    if (nonunit != 1 || fp[axis] <= blk[axis]) {
        return {};
    }
    std::vector<int> free_vec(free.begin(), free.end());
    // Sort: every other snake_1d call site passes a sorted vector; hash-set
    // iteration order must not decide which snake cycle the budgeted search
    // finds (stdlib-dependent placement otherwise).
    std::sort(free_vec.begin(), free_vec.end());
    return FoldEnumerator::snake_1d(free_vec, dims, fp[axis], budget);
}

}  // namespace

std::optional<std::vector<int>> scatter_assign(
    const FoldVariant& v,
    const std::unordered_set<int>& free,
    const std::vector<int>& dims,
    const BlockModel& cm,
    const std::vector<std::pair<int, int>>& ring,
    const std::function<double(const std::array<int, 3>&)>& block_rank,
    int budget) {
    const std::array<int, 3> blk = cm.block_dims();
    const std::array<int, 3>& fp = v.footprint;

    if (auto cyc = snake_fast_path(fp, blk, free, dims, budget); !cyc.empty()) {
        return cyc;
    }

    Tiling t = tile(fp, blk);
    std::array<int, 3> gdim{dims[0] / blk[0], dims[1] / blk[1],
                            dims[2] / blk[2]};
    std::array<int, 3> omax{0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        omax[i] = t.offset_free[i] ? (blk[i] - fp[i]) : 0;
    }

    // `budget` is a per-phase expansion cap: the 1-D snake fast path above uses
    // it as snake_1d's max_depth, and the DFS below uses it for block-trial
    // expansions. The two phases are mutually exclusive on success (snake
    // returns immediately when it finds a cycle), so a failed 1-D job can spend
    // up to budget in each phase.
    int spent = 0;
    for (int oz = 0; oz <= omax[2]; ++oz) {
        for (int oy = 0; oy <= omax[1]; ++oy) {
            for (int ox = 0; ox <= omax[0]; ++ox) {
                std::array<int, 3> O{ox, oy, oz};
                std::set<std::array<int, 3>> used;
                std::map<std::array<int, 3>, std::array<int, 3>> grid2blk;

                std::function<bool(size_t)> dfs = [&](size_t bi) -> bool {
                    if (bi == t.blocks.size()) {
                        return true;
                    }
                    const Block& tb = t.blocks[bi];
                    std::array<int, 3> lo;
                    std::array<int, 3> hi;
                    for (int i = 0; i < 3; ++i) {
                        lo[i] = t.offset_free[i] ? O[i] : 0;
                        hi[i] = lo[i] + tb.shape[i];
                    }
                    std::vector<std::pair<double, std::array<int, 3>>> cands;
                    for (int cz = 0; cz < gdim[2]; ++cz) {
                        for (int cy = 0; cy < gdim[1]; ++cy) {
                            for (int cx = 0; cx < gdim[0]; ++cx) {
                                std::array<int, 3> C{cx, cy, cz};
                                if (used.count(C) > 0) {
                                    continue;
                                }
                                bool ok = true;
                                for (int z = lo[2]; z < hi[2] && ok; ++z) {
                                    for (int y = lo[1]; y < hi[1] && ok; ++y) {
                                        for (int x = lo[0]; x < hi[0] && ok;
                                             ++x) {
                                            if (free.count(cm.id_of(
                                                    {C[0] * blk[0] + x,
                                                     C[1] * blk[1] + y,
                                                     C[2] * blk[2] + z})) ==
                                                0) {
                                                ok = false;
                                            }
                                        }
                                    }
                                }
                                if (ok) {
                                    cands.push_back({block_rank(C), C});
                                }
                            }
                        }
                    }
                    std::sort(cands.begin(), cands.end(),
                              [](const auto& a, const auto& c) {
                                  if (a.first != c.first) {
                                      return a.first < c.first;
                                  }
                                  return a.second < c.second;
                              });
                    for (const auto& pc : cands) {
                        if (++spent > budget) {
                            return false;
                        }
                        used.insert(pc.second);
                        grid2blk[tb.grid] = pc.second;
                        if (dfs(bi + 1)) {
                            return true;
                        }
                        used.erase(pc.second);
                        grid2blk.erase(tb.grid);
                    }
                    return false;
                };

                if (dfs(0)) {
                    auto rm = build_rank_map(v, t, O, dims, blk, grid2blk);
                    if (cm.polarity_free()) {
                        // RDCN mode: DOR-tolerant glue. Ring edges the OCS
                        // cannot serve are not grounds for rejection — like
                        // open contiguous placements they ride the shared
                        // fabric (S3), and the candidate pays in dor_edges
                        // (build_placement wires only the realizable,
                        // port-disjoint subset). This is what makes shapes
                        // with a factor-3 dim (the whole 72-chip family)
                        // glue-placeable at all: their fold turns and ring
                        // closures land on interior chips no circuit can
                        // reach, which under the strict gate vetoed every
                        // 2-tile variant.
                        return rm;
                    }
                    auto oe = FootprintRouter::ocs_edges(ring, rm, cm);
                    // Strict pre-2026-07 gate (rfoldv1 bit-compat): the first
                    // complete assignment decides realizability — whether a
                    // ring wrap-closure lands on opposite block faces is a
                    // fold-variant property (offset- and assignment-
                    // independent), so if any OCS edge is unrealizable here,
                    // no other offset or block assignment would fix it ->
                    // give up this variant (RFold then tries other variants,
                    // else DEFER). E.g. a short odd 1-D ring like 1x1x3
                    // closes onto an interior node and is rejected.
                    for (const auto& e : oe) {
                        if (!cm.ocs_realizable_scatter(e.first, e.second)) {
                            return std::nullopt;
                        }
                    }
                    return rm;
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<std::vector<int>> scatter_assign_nested(
    const FoldVariant& v,
    const std::unordered_set<int>& free,
    const std::vector<int>& dims,
    const BlockModel& cm,
    const std::vector<std::pair<int, int>>& ring,
    int budget) {
    const std::array<int, 3> blk = cm.block_dims();
    const std::array<int, 3>& fp = v.footprint;

    if (auto cyc = snake_fast_path(fp, blk, free, dims, budget); !cyc.empty()) {
        return cyc;
    }

    Tiling t = tile(fp, blk);
    std::array<int, 3> gdim{dims[0] / blk[0], dims[1] / blk[1],
                            dims[2] / blk[2]};
    const int vol = blk[0] * blk[1] * blk[2];

    // Per-block free-chip count: 0 < count < vol marks a dirty block —
    // preferred hosts for partial tiles (concentrate the dirt).
    std::map<std::array<int, 3>, int> free_in_block;
    for (int n : free) {
        ++free_in_block[cm.block_of(n)];
    }

    // taken[] tracks chips claimed by already-placed tiles of THIS job so
    // co-resident tiles stay disjoint.
    std::unordered_set<int> taken;
    // tile -> (block, offset); rank_map derived at the end.
    std::vector<std::pair<std::array<int, 3>, std::array<int, 3>>> pick(
        t.blocks.size());

    int spent = 0;
    std::function<bool(size_t)> dfs = [&](size_t bi) -> bool {
        if (bi == t.blocks.size()) {
            return true;
        }
        const Block& tb = t.blocks[bi];
        const bool full_tile = tb.shape[0] == blk[0] && tb.shape[1] == blk[1] &&
                               tb.shape[2] == blk[2];
        // Candidates: (rank, block, offset). Rank prefers dirty blocks for
        // partial tiles; full tiles need a wholly-free block either way.
        struct Cand {
            double rank;
            std::array<int, 3> C;
            std::array<int, 3> o;
        };
        std::vector<Cand> cands;
        for (int cz = 0; cz < gdim[2]; ++cz) {
            for (int cy = 0; cy < gdim[1]; ++cy) {
                for (int cx = 0; cx < gdim[0]; ++cx) {
                    std::array<int, 3> C{cx, cy, cz};
                    auto it = free_in_block.find(C);
                    const int bf = it == free_in_block.end() ? 0 : it->second;
                    if (bf == 0) {
                        continue;
                    }
                    if (full_tile && bf < vol) {
                        continue;
                    }
                    const bool dirty = bf < vol;
                    for (int oz = 0; oz + tb.shape[2] <= blk[2]; ++oz) {
                        for (int oy = 0; oy + tb.shape[1] <= blk[1]; ++oy) {
                            for (int ox = 0; ox + tb.shape[0] <= blk[0]; ++ox) {
                                bool ok = true;
                                for (int z = 0; z < tb.shape[2] && ok; ++z) {
                                    for (int y = 0; y < tb.shape[1] && ok;
                                         ++y) {
                                        for (int x = 0; x < tb.shape[0] && ok;
                                             ++x) {
                                            int id = cm.id_of(
                                                {C[0] * blk[0] + ox + x,
                                                 C[1] * blk[1] + oy + y,
                                                 C[2] * blk[2] + oz + z});
                                            if (free.count(id) == 0 ||
                                                taken.count(id) > 0) {
                                                ok = false;
                                            }
                                        }
                                    }
                                }
                                if (ok) {
                                    // Dirty blocks first, then origin-most
                                    // offsets, then block order (determinism).
                                    cands.push_back(
                                        {dirty ? 0.0 : 1.0, C, {ox, oy, oz}});
                                }
                            }
                        }
                    }
                }
            }
        }
        std::stable_sort(cands.begin(), cands.end(),
                         [](const Cand& a, const Cand& b) {
                             if (a.rank != b.rank) {
                                 return a.rank < b.rank;
                             }
                             if (a.C != b.C) {
                                 return a.C < b.C;
                             }
                             return a.o < b.o;
                         });
        for (const auto& c : cands) {
            if (++spent > budget) {
                return false;
            }
            std::vector<int> ids;
            ids.reserve(static_cast<size_t>(tb.shape[0]) * tb.shape[1] *
                        tb.shape[2]);
            for (int z = 0; z < tb.shape[2]; ++z) {
                for (int y = 0; y < tb.shape[1]; ++y) {
                    for (int x = 0; x < tb.shape[0]; ++x) {
                        ids.push_back(cm.id_of({c.C[0] * blk[0] + c.o[0] + x,
                                                c.C[1] * blk[1] + c.o[1] + y,
                                                c.C[2] * blk[2] + c.o[2] + z}));
                    }
                }
            }
            for (int id : ids) {
                taken.insert(id);
            }
            pick[bi] = {c.C, c.o};
            if (dfs(bi + 1)) {
                return true;
            }
            for (int id : ids) {
                taken.erase(id);
            }
        }
        return false;
    };

    if (!dfs(0)) {
        return std::nullopt;
    }
    std::vector<int> rm(v.embedding.size());
    for (size_t r = 0; r < v.embedding.size(); ++r) {
        const auto& e = v.embedding[r];
        std::array<int, 3> g{e[0] / blk[0], e[1] / blk[1], e[2] / blk[2]};
        // BlockTiler emits tiles in gz-major, gx-fastest order.
        const size_t ti = (static_cast<size_t>(g[2]) * t.grid_dims[1] + g[1]) *
                              t.grid_dims[0] +
                          g[0];
        const auto& [C, o] = pick[ti];
        std::array<int, 3> gc;
        for (int i = 0; i < 3; ++i) {
            gc[i] = C[i] * blk[i] + o[i] + (e[i] % blk[i]);
        }
        rm[r] = gc[2] * dims[0] * dims[1] + gc[1] * dims[0] + gc[0];
    }
    return rm;
}

}  // namespace Scheduling
}  // namespace AstraSim
