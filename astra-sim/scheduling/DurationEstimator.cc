/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/DurationEstimator.hh"

#include "astra-sim/scheduling/ChakraTrace.hh"
#include "astra-sim/scheduling/JobInstance.hh"

#include "extern/graph_frontend/chakra/src/feeder_v3/common.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>

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
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return 0;
    }
    ChakraGlobalMetadata gm;
    if (!read_message<ChakraGlobalMetadata>(f, gm)) {
        return 0;
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

std::unique_ptr<DurationEstimator> make_duration_estimator(
    const std::string& name,
    double peak_perf,
    double local_mem_bw,
    double max_link_bw) {
    if (name == "roofline-comm") {
        return std::make_unique<RooflineCommEstimator>(peak_perf, local_mem_bw,
                                                       max_link_bw);
    }
    return nullptr;
}

}  // namespace Scheduling
}  // namespace AstraSim
