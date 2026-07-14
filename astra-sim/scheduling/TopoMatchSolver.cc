/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/TopoMatchSolver.hh"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

extern "C" {
#include <topomatch.h>
// TopoMatch maps onto a torus3D via Scotch, which draws from a process-global
// RNG that advances on every mapping. Reset it before each solve so a placement
// is a pure, reproducible function of (free_ids, affinity), independent of how
// many jobs were placed before it. Declared here in case topomatch.h was built
// without the HAVE_LIBSCOTCH path that would pull in <scotch.h>.
void SCOTCH_randomReset(void);
}

namespace AstraSim {
namespace Scheduling {

TopoMatchSolver::TopoMatchSolver(int W, int L, int H) : W_(W), L_(L), H_(H) {
    // PID-suffixed so concurrent sims on one host never share the file: a
    // late-starting process truncating a shared path while another is mid
    // SCOTCH_archLoad yields a garbage arch and a segfault (topomatch ignores
    // archLoad's return value).
    arch_path_ = "/tmp/astrasim_topomatch_torus3D_" + std::to_string(W_) + "x" +
                 std::to_string(L_) + "x" + std::to_string(H_) + "_pid" +
                 std::to_string(getpid()) + ".tgt";
    std::ofstream out(arch_path_);
    out << "torus3D " << W_ << " " << L_ << " " << H_;
    out.flush();
    arch_written_ = static_cast<bool>(out);
    out.close();

    tm_set_max_nb_threads(1);
    tm_set_exhaustive_search_flag(0);
    tm_set_verbose_level(0);
}

TopoMatchSolver::~TopoMatchSolver() {
    std::remove(arch_path_.c_str());
    tm_finalize();
}

std::optional<std::vector<int>> TopoMatchSolver::solve(
    const std::vector<int>& free_ids,
    const std::vector<std::vector<double>>& affinity) {
    const int K = static_cast<int>(affinity.size());
    if (K == 0 || !arch_written_) {
        return std::nullopt;
    }
    if (static_cast<int>(free_ids.size()) < K) {
        return std::nullopt;
    }

    // libscotch-6.1's SCOTCH_archSub (_SCOTCHarchSubArchBuild) segfaults when
    // the binding-constraint set holds a single node: it cannot build a
    // one-node sub-architecture of the torus3D. This fires deterministically
    // once the cluster fills to its last free node and a single-rank job is
    // placed (the free_ids.size() >= K guard above forces K == 1 here). The
    // mapping is trivial in that case -- the one rank must occupy the one free
    // node -- so short-circuit before entering Scotch. Verified in isolation: N
    // == 1 crashes inside Scotch while N >= 2 maps cleanly.
    if (free_ids.size() == 1) {
        return std::vector<int>{free_ids.front()};
    }

    // Reproducible mapping: reset Scotch's global RNG to its initial state so
    // repeated solves with identical inputs yield identical placements.
    SCOTCH_randomReset();

    tm_topology_t* topology = tm_load_topology(
        const_cast<char*>(arch_path_.c_str()), TM_FILE_TYPE_TGT);
    if (!topology) {
        return std::nullopt;
    }

    std::vector<int> ids = free_ids;  // copied internally by TopoMatch
    if (!tm_topology_set_binding_constraints(
            ids.data(), static_cast<int>(ids.size()), topology)) {
        tm_free_topology(topology);
        return std::nullopt;
    }

    // malloc the matrix; ownership transfers to tm_free_affinity_mat (free()).
    double** mat = static_cast<double**>(malloc(sizeof(double*) * K));
    if (mat == nullptr) {
        return std::nullopt;
    }
    for (int i = 0; i < K; ++i) {
        mat[i] = static_cast<double*>(malloc(sizeof(double) * K));
        if (mat[i] == nullptr) {
            for (int r = 0; r < i; ++r) {
                free(mat[r]);
            }
            free(mat);
            return std::nullopt;
        }
        for (int j = 0; j < K; ++j) {
            mat[i][j] = affinity[i][j];
        }
    }
    // tm_build_affinity_mat takes ownership of `mat` (and its rows) without
    // copying; tm_free_affinity_mat releases them below. The static analyzer
    // can't see through the libtopomatch boundary, so it false-positives a leak
    // here; suppress that one check (the memory is freed).
    // NOLINTNEXTLINE(clang-analyzer-unix.Malloc)
    tm_affinity_mat_t* aff = tm_build_affinity_mat(mat, K);
    tm_solution_t* sol = tm_compute_mapping(topology, aff, nullptr, nullptr);

    std::optional<std::vector<int>> result;
    if (sol && sol->sigma && static_cast<int>(sol->sigma_length) == K) {
        std::set<int> allowed(free_ids.begin(), free_ids.end());
        std::set<int> seen;
        std::vector<int> sigma(K);
        bool ok = true;
        for (int i = 0; i < K; ++i) {
            const int node = sol->sigma[i];
            if (allowed.count(node) == 0 || !seen.insert(node).second) {
                ok = false;
                break;
            }
            sigma[i] = node;
        }
        if (ok) {
            result = std::move(sigma);
        }
    }

    tm_free_topology(topology);
    if (sol) {
        tm_free_solution(sol);
    }
    tm_free_affinity_mat(aff);  // frees mat[i], mat, sum_row, struct
    return result;
}

}  // namespace Scheduling
}  // namespace AstraSim
