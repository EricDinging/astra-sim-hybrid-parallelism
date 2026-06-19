/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/AffinityMatrix.hh"
#include "astra-sim/scheduling/ChakraTrace.hh"

#include "extern/graph_frontend/chakra/src/feeder_v3/common.h"
#include <json/json.hpp>

#include <algorithm>
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
    std::vector<std::vector<double>> mat(K, std::vector<double>(K, 0.0));
    for (int n = 0; n < K; ++n) {
        const RankComm& rc = per_rank[n];
        // std::map iterates in key order, matching python's sorted(...).
        for (const auto& [pg_name, vol_bytes] : rc.coll_bytes) {
            auto git = comm_group.find(pg_name);
            if (git == comm_group.end() || git->second.empty()) {
                continue;
            }
            std::vector<int> sorted_group = git->second;
            std::sort(sorted_group.begin(), sorted_group.end());
            int dst = find_next_wrap(sorted_group, n);
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

std::optional<std::vector<std::vector<double>>> build_affinity_matrix(
    const std::string& trace_dir, int K) {
    namespace fs = std::filesystem;

    // 1. comm_group.json -> map<pg_name, ranks>.
    std::map<std::string, std::vector<int>> comm_group;
    {
        std::ifstream in(trace_dir + "/comm_group.json");
        if (!in) {
            return std::nullopt;
        }
        json j;
        try {
            in >> j;
        } catch (...) {
            return std::nullopt;
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::vector<int> ranks;
            for (const auto& r : it.value()) {
                ranks.push_back(r.get<int>());
            }
            comm_group[it.key()] = std::move(ranks);
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
    std::vector<RankComm> per_rank(K);
    int expected = 0;
    for (const auto& [rank, path] : et_by_rank) {
        if (rank != expected) {
            return std::nullopt;
        }
        RankComm& rc = per_rank[expected];
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

    return assemble_matrix(K, per_rank, comm_group);
}

}  // namespace Scheduling
}  // namespace AstraSim
