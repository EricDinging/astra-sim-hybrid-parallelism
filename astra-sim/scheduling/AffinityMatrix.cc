/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/AffinityMatrix.hh"
#include "astra-sim/common/Logging.hh"
#include "astra-sim/scheduling/ChakraTrace.hh"

#include "extern/graph_frontend/chakra/src/feeder_v3/common.h"
#include <json/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace AstraSim {
namespace Scheduling {

using json = nlohmann::json;
using Chakra::FeederV3::ChakraGlobalMetadata;
using Chakra::FeederV3::ChakraNode;

int find_next_wrap(const std::vector<int>& sorted_group, int rank) {
    auto it = std::upper_bound(sorted_group.begin(), sorted_group.end(), rank);
    if (it == sorted_group.end()) {
        return sorted_group.front();
    }
    return *it;
}

std::vector<std::vector<double>> assemble_matrix(
    int K,
    const std::vector<RankComm>& per_rank,
    const std::map<std::string, std::vector<int>>& comm_group) {
    // Precondition: every group's member list is sorted ascending
    // (find_next_wrap needs it). build_affinity_matrix sorts at parse time;
    // re-sorting here per rank per group cost O(K^2 log K) on wide groups.
    for ([[maybe_unused]] const auto& [pg_name, ranks] : comm_group) {
        assert(std::is_sorted(ranks.begin(), ranks.end()));
    }
    std::vector<std::vector<double>> mat(K, std::vector<double>(K, 0.0));
    for (int n = 0; n < K; ++n) {
        const RankComm& rc = per_rank[n];
        // std::map iterates in key order, matching python's sorted(...).
        for (const auto& [pg_name, vol_bytes] : rc.coll_bytes) {
            auto git = comm_group.find(pg_name);
            if (git == comm_group.end() || git->second.empty()) {
                continue;
            }
            int dst = find_next_wrap(git->second, n);
            // Defensive mirror of the send-path guard below: group members
            // are validated at parse time in build_affinity_matrix, but this
            // write must stay safe for any future caller.
            if (dst < 0 || dst >= K) {
                continue;
            }
            mat[n][dst] += static_cast<double>(vol_bytes / 1000000ULL);
        }
        for (const auto& [dst, vol_bytes] : rc.send_bytes) {
            if (dst < 0 || dst >= K) {
                continue;
            }
            mat[n][dst] += static_cast<double>(vol_bytes / 1000000ULL);
        }
    }
    return mat;
}

namespace {

// One directory's parsed traces: the per-rank communication summaries plus
// the (sorted-member) comm groups. A few hundred KB even for 4096 ranks --
// unlike the K x K matrix (134 MB at K=4096), which is why the cache stores
// summaries and assemble_matrix() re-runs per placement.
struct ParsedTraces {
    std::vector<RankComm> per_rank;
    std::map<std::string, std::vector<int>> comm_group;
};

// Parse trace_dir from scratch (comm_group.json + K chakra_trace.*.et).
// Factored out of build_affinity_matrix so the cache below can wrap it.
std::optional<ParsedTraces> parse_trace_dir(const std::string& trace_dir,
                                            int K) {
    namespace fs = std::filesystem;

    ParsedTraces parsed;
    auto& comm_group = parsed.comm_group;

    // 1. comm_group.json -> map<pg_name, ranks>.
    {
        std::ifstream in(trace_dir + "/comm_group.json");
        if (!in) {
            return std::nullopt;
        }
        json j;
        try {
            in >> j;
            for (auto it = j.begin(); it != j.end(); ++it) {
                std::vector<int> ranks;
                for (const auto& r : it.value()) {
                    const int rank = r.get<int>();
                    // Group members are job-local ranks and index K-wide
                    // matrix rows in assemble_matrix; an out-of-range member
                    // (e.g. a global NPU id) would corrupt the heap.
                    if (rank < 0 || rank >= K) {
                        LoggerFactory::get_logger("scheduling")
                            ->warn("{}/comm_group.json: group {} member {} "
                                   "outside [0, {}); rejecting affinity "
                                   "matrix",
                                   trace_dir, it.key(), rank, K);
                        return std::nullopt;
                    }
                    ranks.push_back(rank);
                }
                // Sorted once here; assemble_matrix requires sorted members
                // (it used to copy + sort per rank per group).
                std::sort(ranks.begin(), ranks.end());
                comm_group[it.key()] = std::move(ranks);
            }
        } catch (...) {
            return std::nullopt;
        }
    }

    // 2. Enumerate chakra_trace.<n>.et files -> map<rank, path>.
    std::map<int, std::string> et_by_rank;
    std::error_code ec;
    auto dir = fs::directory_iterator(trace_dir, ec);
    if (ec) {
        return std::nullopt;
    }
    for (const auto& entry : dir) {
        const std::string name = entry.path().filename().string();
        if (name.size() < 4 || name.substr(name.size() - 3) != ".et") {
            continue;
        }
        const std::string stem = name.substr(0, name.size() - 3);
        const auto dot = stem.find_last_of('.');
        if (dot == std::string::npos) {
            continue;
        }
        try {
            int n = std::stoi(stem.substr(dot + 1));
            et_by_rank[n] = entry.path().string();
        } catch (...) {
            continue;
        }
    }
    if (static_cast<int>(et_by_rank.size()) != K) {
        return std::nullopt;
    }

    // 3. Decode each rank's trace into RankComm. Require ranks 0..K-1.
    parsed.per_rank.resize(K);
    int expected = 0;
    for (const auto& [rank, path] : et_by_rank) {
        if (rank != expected) {
            return std::nullopt;
        }
        RankComm& rc = parsed.per_rank[expected];
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return std::nullopt;
        }
        ChakraGlobalMetadata gm;
        if (!read_message<ChakraGlobalMetadata>(f, gm)) {
            return std::nullopt;
        }
        ChakraNode node;
        while (read_message<ChakraNode>(f, node)) {
            if (node.type() == ChakraProtoMsg::COMM_COLL_NODE) {
                std::string pg_name;
                uint64_t comm_size = 0;
                bool have_pg = false;
                for (int a = 0; a < node.attr_size(); ++a) {
                    const auto& attr = node.attr(a);
                    if (attr.name() == "pg_name") {
                        pg_name = attr.string_val();
                        have_pg = true;
                    } else if (attr.name() == "comm_size") {
                        comm_size = static_cast<uint64_t>(attr.int64_val());
                    }
                }
                if (have_pg) {
                    rc.coll_bytes[pg_name] += comm_size;
                }
            } else if (node.type() == ChakraProtoMsg::COMM_SEND_NODE) {
                int comm_dst = -1;
                uint64_t comm_size = 0;
                bool have_dst = false;
                for (int a = 0; a < node.attr_size(); ++a) {
                    const auto& attr = node.attr(a);
                    if (attr.name() == "comm_dst") {
                        comm_dst = attr.int32_val();
                        have_dst = true;
                    } else if (attr.name() == "comm_size") {
                        comm_size = static_cast<uint64_t>(attr.int64_val());
                    }
                }
                if (have_dst) {
                    rc.send_bytes[comm_dst] += comm_size;
                }
            }
        }
        ++expected;
    }

    return parsed;
}

}  // namespace

std::optional<std::vector<std::vector<double>>> build_affinity_matrix(
    const std::string& trace_dir, int K) {
    namespace fs = std::filesystem;

    // Job dirs are commonly symlinks into a shared tracelib (a few hundred
    // distinct trace directories serve thousands of job instances), so the
    // parse -- K protobuf files, the dominant cost here -- is cached per
    // RESOLVED directory. Only successful parses are cached: a failure may
    // be transient (e.g. fd exhaustion) and must not poison every job
    // sharing the directory. The matrix itself is rebuilt per call; the
    // solver symmetrizes it in place, so callers need a fresh copy anyway.
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(trace_dir, ec);
    const std::string key_dir = ec ? trace_dir : canon.string();

    static std::map<std::pair<std::string, int>, ParsedTraces> cache;
    auto it = cache.find({key_dir, K});
    if (it == cache.end()) {
        auto parsed = parse_trace_dir(trace_dir, K);
        if (!parsed) {
            return std::nullopt;
        }
        it =
            cache.emplace(std::make_pair(key_dir, K), std::move(*parsed)).first;
    }
    return assemble_matrix(K, it->second.per_rank, it->second.comm_group);
}

}  // namespace Scheduling
}  // namespace AstraSim
