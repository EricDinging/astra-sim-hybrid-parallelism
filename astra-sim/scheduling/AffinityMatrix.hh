/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_AFFINITYMATRIX_HH__
#define __ASTRASIM_SCHEDULING_AFFINITYMATRIX_HH__

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// Per-rank communication volumes extracted from one Chakra .et trace.
struct RankComm {
    std::map<std::string, uint64_t>
        coll_bytes;                      // pg_name -> summed COMM_COLL bytes
    std::map<int, uint64_t> send_bytes;  // comm_dst -> summed COMM_SEND bytes
};

// First group member strictly greater than `rank`, wrapping to
// sorted_group.front() if none. Precondition: sorted_group sorted & non-empty.
// Mirrors topomatch_prep.py::find_next_wrap (bisect_right semantics).
int find_next_wrap(const std::vector<int>& sorted_group, int rank);

// Assemble the K x K directed traffic matrix (integer MB stored as double).
// Collective volume for a pg is summed in bytes then attributed to the single
// edge (rank -> find_next_wrap(group, rank)); p2p send volume to (rank -> dst);
// both converted to MB via integer truncation (bytes / 1'000'000) per edge and
// accumulated. Mirrors topomatch_prep.py. Precondition: every group's member
// list is sorted ascending (asserted; build_affinity_matrix sorts at parse).
std::vector<std::vector<double>> assemble_matrix(
    int K,
    const std::vector<RankComm>& per_rank,
    const std::map<std::string, std::vector<int>>& comm_group);

// Read K chakra_trace.*.et files + comm_group.json from trace_dir, decode the
// per-rank volumes, and return assemble_matrix(...). Returns nullopt if the
// directory/comm_group.json is unreadable/invalid, the .et count != K, or the
// rank indices are not exactly 0..K-1. The decoded summaries are cached per
// symlink-resolved directory for the process lifetime (job dirs are symlinks
// into a shared tracelib), so only the first job per distinct directory pays
// the parse; the returned matrix is freshly assembled on every call.
std::optional<std::vector<std::vector<double>>> build_affinity_matrix(
    const std::string& trace_dir, int K);

}  // namespace Scheduling
}  // namespace AstraSim

#endif
