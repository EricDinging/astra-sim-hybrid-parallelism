/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/DurationEstimator.hh"

#include "astra-sim/common/Logging.hh"
#include "astra-sim/scheduling/ChakraTrace.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include "extern/graph_frontend/chakra/src/feeder_v3/common.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace AstraSim {
namespace Scheduling {

using Chakra::FeederV3::ChakraGlobalMetadata;
using Chakra::FeederV3::ChakraNode;

namespace {

// Integer attribute value by name. AttributeProto is a oneof, so the unused
// integer field reads back as 0; take whichever is set. stage encodes num_ops
// and comm_size as int64, tensor_size as uint64.
uint64_t attr_u64(const ChakraNode& node, const char* name) {
    for (int i = 0; i < node.attr_size(); ++i) {
        const auto& a = node.attr(i);
        if (a.name() == name) {
            if (a.int64_val() != 0) {
                return static_cast<uint64_t>(a.int64_val());
            }
            return a.uint64_val();
        }
    }
    return 0;
}

}  // namespace

RooflineCommEstimator::RooflineCommEstimator(double peak_perf,
                                             double local_mem_bw,
                                             double max_link_bw)
    : peak_perf_(peak_perf),
      local_mem_bw_(local_mem_bw),
      max_link_bw_(max_link_bw) {}

Tick RooflineCommEstimator::estimate(const JobInstance& job) const {
    const std::string path = job.trace_dir + "/chakra_trace.0.et";
    // Fail fast on an unreadable trace instead of returning 0: a silent zero
    // is indistinguishable from a real estimate and corrupts EASY's
    // reservation math -- a running job with est 0 appears to free its NPUs
    // immediately, and a pending candidate with est 0 is always
    // backfill-safe, degrading EASY into unbounded aggressive backfill that
    // can starve the pivot.
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        LoggerFactory::get_logger("scheduling")
            ->critical("job {}: cannot read {} for duration estimation",
                       job.job_id, path);
        std::exit(1);
    }
    ChakraGlobalMetadata gm;
    if (!read_message<ChakraGlobalMetadata>(f, gm)) {
        LoggerFactory::get_logger("scheduling")
            ->critical("job {}: {} is truncated or not a Chakra trace",
                       job.job_id, path);
        std::exit(1);
    }

    double compute_ns = 0.0;
    double comm_ns = 0.0;
    ChakraNode node;
    while (read_message<ChakraNode>(f, node)) {
        if (node.type() == ChakraProtoMsg::COMP_NODE) {
            const double num_ops =
                static_cast<double>(attr_u64(node, "num_ops"));
            const double tensor_size =
                static_cast<double>(attr_u64(node, "tensor_size"));
            if (tensor_size <= 0.0 || num_ops <= 0.0) {
                continue;  // invalid in roofline mode (matches Workload.cc)
            }
            const double oi = num_ops / tensor_size;
            // Roofline: min(mem_bw * oi, peak). Mirrors Roofline::get_perf.
            const double perf = std::min(local_mem_bw_ * oi, peak_perf_);
            compute_ns += num_ops / perf * 1e9;
        } else if (node.type() == ChakraProtoMsg::COMM_COLL_NODE ||
                   node.type() == ChakraProtoMsg::COMM_SEND_NODE) {
            const double bytes =
                static_cast<double>(attr_u64(node, "comm_size"));
            comm_ns += bytes / max_link_bw_ * 1e9;
        }
    }
    return static_cast<Tick>(compute_ns + comm_ns);
}

SvcTableEstimator::SvcTableEstimator(const std::string& csv_path) {
    auto logger = LoggerFactory::get_logger("scheduling");
    std::ifstream f(csv_path);
    if (!f) {
        logger->critical("svc-table estimator: cannot read {}", csv_path);
        std::exit(1);
    }
    auto split = [](const std::string& line) {
        std::vector<std::string> cols;
        std::stringstream ss(line);
        for (std::string c; std::getline(ss, c, ',');) {
            // Tolerate CRLF files and stray spaces around fields.
            c.erase(c.find_last_not_of(" \t\r\n") + 1);
            c.erase(0, c.find_first_not_of(" \t"));
            cols.push_back(c);
        }
        return cols;
    };
    std::string line;
    std::getline(f, line);
    const auto header = split(line);
    const auto col = [&](const char* want) {
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (header[i] == want) {
                return static_cast<int>(i);
            }
        }
        logger->critical("svc-table estimator: {} lacks a '{}' column",
                         csv_path, want);
        std::exit(1);
    };
    const int shape_col = col("shape");
    const int svc_col = col("svc_per_iter_ns");
    while (std::getline(f, line)) {
        const auto cols = split(line);
        if (static_cast<int>(cols.size()) <= std::max(shape_col, svc_col)) {
            continue;  // blank / short row
        }
        svc_per_iter_[cols[shape_col]] =
            static_cast<Tick>(std::stoull(cols[svc_col]));
    }
}

Tick SvcTableEstimator::estimate(const JobInstance& job) const {
    const std::string shape = std::to_string(job.shape[0]) + "x" +
                              std::to_string(job.shape[1]) + "x" +
                              std::to_string(job.shape[2]);
    const auto it = svc_per_iter_.find(shape);
    if (it == svc_per_iter_.end()) {
        LoggerFactory::get_logger("scheduling")
            ->critical("job {}: shape {} missing from the service-time table",
                       job.job_id, shape);
        std::exit(1);
    }
    return it->second * static_cast<Tick>(std::max(1, job.num_iterations));
}

std::unique_ptr<DurationEstimator> make_duration_estimator(
    const std::string& name,
    double peak_perf,
    double local_mem_bw,
    double max_link_bw,
    const std::string& svc_table_path) {
    if (name == "roofline-comm") {
        return std::make_unique<RooflineCommEstimator>(peak_perf, local_mem_bw,
                                                       max_link_bw);
    }
    if (name == "svc-table") {
        return std::make_unique<SvcTableEstimator>(svc_table_path);
    }
    return nullptr;
}

}  // namespace Scheduling
}  // namespace AstraSim
