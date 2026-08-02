/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "reconfigurable/Router.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <utility>

using namespace NetworkAnalyticalReconfigurable;

Router::Router(std::shared_ptr<Topology> topology,
               std::vector<int> npus_per_dim,
               bool is_torus,
               bool bidi,
               int devices_count,
               const std::unordered_set<int>* failed_npus,
               std::size_t budget_bytes,
               bool fullmesh) noexcept
    : topology_(std::move(topology)),
      npus_per_dim_(std::move(npus_per_dim)),
      dims_count_(static_cast<int>(npus_per_dim_.size())),
      is_torus_(is_torus),
      bidi_(bidi),
      fullmesh_(fullmesh),
      devices_count_(devices_count),
      failed_npus_(failed_npus),
      cur_bytes_(0),
      budget_bytes_(budget_bytes),
      // DOR-walk scratch is never touched in fullmesh mode; skip the O(N) alloc.
      visited_(fullmesh ? 0 : static_cast<std::size_t>(devices_count), -1),
      epoch_(0) {
    assert(dims_count_ > 0 || fullmesh_);
}

const RoutePtr& Router::lookup(DeviceId src, DeviceId dst) const noexcept {
    const std::uint64_t k = key(src, dst);

    // Overrides exist only after apply_job_wiring; skip the probe entirely on
    // the common no-override runs.
    if (!overrides_.empty()) {
        const auto oit = overrides_.find(k);
        if (oit != overrides_.end()) {
            return oit->second;
        }
    }

    const auto cit = cache_.find(k);
    if (cit != cache_.end()) {
        return cit->second;
    }

    // Miss: compute, account, insert. Over budget the whole cache is dropped
    // (before inserting, so the fresh entry survives): routes are pure and
    // deterministically recomputable, so the eviction policy is unobservable.
    auto r = std::make_shared<const Route>(fullmesh_ ? direct_route(src, dst) : compute_dor(src, dst));
    const auto bytes = route_bytes(*r);
    if (cur_bytes_ + bytes > budget_bytes_) {
        clear_cache();
    }
    cur_bytes_ += bytes;
    return cache_.emplace(k, std::move(r)).first->second;
}

void Router::set_override(DeviceId s, DeviceId t, Route route) noexcept {
    invalidate(s, t);
    overrides_[key(s, t)] = std::make_shared<const Route>(std::move(route));
}

void Router::erase_override(DeviceId s, DeviceId t) noexcept {
    overrides_.erase(key(s, t));
}

void Router::invalidate(DeviceId s, DeviceId t) const noexcept {
    const auto cit = cache_.find(key(s, t));
    if (cit != cache_.end()) {
        cur_bytes_ -= route_bytes(*cit->second);
        cache_.erase(cit);
    }
}

void Router::clear() noexcept {
    overrides_.clear();
    clear_cache();
}

void Router::clear_cache() const noexcept {
    cache_.clear();
    cur_bytes_ = 0;
}

void Router::set_budget_bytes(std::size_t bytes) noexcept {
    budget_bytes_ = bytes;
    if (cur_bytes_ > budget_bytes_) {
        clear_cache();
    }
}

Route Router::direct_route(DeviceId src, DeviceId dst) const noexcept {
    // On a complete graph the BFS shortest path is exactly [src, dst];
    // TopologyManager::set_fullmesh() validated every off-diagonal cell.
    if (src == dst) {
        return {topology_->get_device(src)};
    }
    return {topology_->get_device(src), topology_->get_device(dst)};
}

Route Router::compute_dor(DeviceId src, DeviceId dst) const noexcept {
    // ---- Lifted verbatim from TopologyManager::precomputeRoutes_DOR(). ----
    auto id_to_coord = [&](int id) -> std::vector<int> {
        std::vector<int> coord(dims_count_);
        int remaining = id;
        for (int d = 0; d < dims_count_; ++d) {
            coord[d] = remaining % npus_per_dim_[d];
            remaining /= npus_per_dim_[d];
        }
        return coord;
    };

    auto coord_to_id = [&](const std::vector<int>& coord) -> int {
        int id = 0;
        int stride = 1;
        for (int d = 0; d < dims_count_; ++d) {
            id += coord[d] * stride;
            stride *= npus_per_dim_[d];
        }
        return id;
    };

    auto is_failed = [&](int id) -> bool {
        return failed_npus_ != nullptr && failed_npus_->find(id) != failed_npus_->end();
    };

    // Directed +1/-1 step for dimension d toward dst, matching the baseline DOR
    // arc selection (unidirectional +1 on a torus unless bidi; sign on a mesh).
    auto dir_step = [&](int d, int cur_d, int dst_d) -> int {
        const int Nd = npus_per_dim_[d];
        if (is_torus_) {
            const int forward = ((dst_d - cur_d) + Nd) % Nd;
            const int backward = Nd - forward;
            if (bidi_) {
                return (forward <= backward) ? +1 : -1;
            }
            return +1;
        }
        return (dst_d > cur_d) ? +1 : -1;
    };

    // Fixed, deterministic detour preference: +x, +y, +z, then -x, -y, -z.
    static const int PREF_DIM[6] = {0, 1, 2, 0, 1, 2};
    static const int PREF_SGN[6] = {+1, +1, +1, -1, -1, -1};

    const int s = src;
    const int t = dst;

    if (s == t) {
        return Route{topology_->get_device(s)};
    }

    Route path;
    std::vector<int> cur = id_to_coord(s);
    const std::vector<int> dst_coord = id_to_coord(t);
    int cur_id = coord_to_id(cur);
    path.push_back(topology_->get_device(cur_id));

    // Failed endpoints never send/recv; store a trivial stub and skip the walk.
    if (is_failed(s) || is_failed(t)) {
        path.push_back(topology_->get_device(t));
        return path;
    }

    ++epoch_;
    visited_[cur_id] = epoch_;

    while (cur != dst_coord) {
        // Happy path == baseline DOR: lowest misaligned dim, one step.
        int d = 0;
        while (d < dims_count_ && cur[d] == dst_coord[d]) {
            ++d;
        }
        const int Nd = npus_per_dim_[d];
        const int step = dir_step(d, cur[d], dst_coord[d]);
        std::vector<int> nxt = cur;
        nxt[d] = (nxt[d] + step + Nd) % Nd;
        const int nxt_id = coord_to_id(nxt);
        if (!is_failed(nxt_id) && visited_[nxt_id] != epoch_) {
            cur = std::move(nxt);
            cur_id = nxt_id;
            visited_[cur_id] = epoch_;
            path.push_back(topology_->get_device(cur_id));
            continue;
        }

        // Detour: first alive, unvisited neighbor in the fixed order.
        bool moved = false;
        for (int k = 0; k < 6; ++k) {
            const int q = PREF_DIM[k];
            if (q >= dims_count_) {
                continue;
            }
            const int Nq = npus_per_dim_[q];
            const int raw = cur[q] + PREF_SGN[k];
            if (!is_torus_ && (raw < 0 || raw >= Nq)) {
                continue;  // mesh has no wrap-around link
            }
            std::vector<int> cand = cur;
            cand[q] = (raw + Nq) % Nq;
            const int cand_id = coord_to_id(cand);
            if (!is_failed(cand_id) && visited_[cand_id] != epoch_) {
                cur = std::move(cand);
                cur_id = cand_id;
                visited_[cur_id] = epoch_;
                path.push_back(topology_->get_device(cur_id));
                moved = true;
                break;
            }
        }

        if (!moved) {
            // Non-backtracking local sidestep can dead-end on a routable pair;
            // this is the documented ~1% failure-rate ceiling (no BFS fallback
            // by design).
            std::cerr << "[Error] (Router) fault-aware DOR: no detour from src=" << s << " to dst=" << t
                      << " (boxed in at node " << cur_id << ")" << std::endl;
            std::exit(1);
        }
    }

    return path;
}
