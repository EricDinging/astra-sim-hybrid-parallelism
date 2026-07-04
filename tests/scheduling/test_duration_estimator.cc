/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "extern/graph_frontend/chakra/schema/protobuf/et_def.pb.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::RooflineCommEstimator;

void write_varint32(std::ostream& f, uint32_t value) {
    uint8_t byte;
    while (value > 0x7f) {
        byte = static_cast<uint8_t>((value & 0x7f) | 0x80);
        f.write(reinterpret_cast<char*>(&byte), 1);
        value >>= 7;
    }
    byte = static_cast<uint8_t>(value);
    f.write(reinterpret_cast<char*>(&byte), 1);
}

void write_delimited(std::ostream& f,
                     const google::protobuf::MessageLite& msg) {
    std::string buf;
    msg.SerializeToString(&buf);
    write_varint32(f, static_cast<uint32_t>(buf.size()));
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
}

void write_et(const std::string& path,
              const std::vector<ChakraProtoMsg::Node>& nodes) {
    std::ofstream f(path, std::ios::binary);
    ChakraProtoMsg::GlobalMetadata gm;
    gm.set_version("0.0.4");
    write_delimited(f, gm);
    for (const auto& n : nodes) {
        write_delimited(f, n);
    }
}

void add_int64_attr(ChakraProtoMsg::Node* n,
                    const std::string& name,
                    int64_t v) {
    auto* a = n->add_attr();
    a->set_name(name);
    a->set_int64_val(v);
}

void add_uint64_attr(ChakraProtoMsg::Node* n,
                     const std::string& name,
                     uint64_t v) {
    auto* a = n->add_attr();
    a->set_name(name);
    a->set_uint64_val(v);
}

// peak=275 TFLOP/s, mem_bw=1200 GB/s, link_bw=50 GB/s (the demo's config).
constexpr double kPeak = 275e12;
constexpr double kMemBw = 1200e9;
constexpr double kLinkBw = 50e9;

TEST(RooflineCommEstimator, ComputePlusCommFromTrace) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "astrasim_dur_fixture";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::vector<ChakraProtoMsg::Node> nodes;

    // COMP node A: num_ops=1.2e12, tensor_size=1.2e10 -> oi=100.
    //   perf = min(1200e9*100=1.2e14, 2.75e14) = 1.2e14 (memory-bound).
    //   t = 1.2e12 / 1.2e14 = 0.01 s = 1e7 ns.
    {
        ChakraProtoMsg::Node n;
        n.set_id(1);
        n.set_type(ChakraProtoMsg::COMP_NODE);
        add_int64_attr(&n, "num_ops", 1200000000000LL);      // 1.2e12
        add_uint64_attr(&n, "tensor_size", 12000000000ULL);  // 1.2e10
        nodes.push_back(n);
    }
    // COMP node B: num_ops=5.5e14, tensor_size=1e12 -> oi=550.
    //   perf = min(1200e9*550=6.6e14, 2.75e14) = 2.75e14 (compute-bound).
    //   t = 5.5e14 / 2.75e14 = 2.0 s = 2e9 ns.
    {
        ChakraProtoMsg::Node n;
        n.set_id(2);
        n.set_type(ChakraProtoMsg::COMP_NODE);
        add_int64_attr(&n, "num_ops", 550000000000000LL);      // 5.5e14
        add_uint64_attr(&n, "tensor_size", 1000000000000ULL);  // 1e12
        nodes.push_back(n);
    }
    // COMM_COLL node: comm_size=5e11 bytes -> 5e11/50e9 = 10 s = 1e10 ns.
    {
        ChakraProtoMsg::Node n;
        n.set_id(3);
        n.set_type(ChakraProtoMsg::COMM_COLL_NODE);
        add_int64_attr(&n, "comm_size", 500000000000LL);  // 5e11
        nodes.push_back(n);
    }

    write_et((dir / "chakra_trace.0.et").string(), nodes);

    JobArrival a{/*job_id=*/0, /*arrival=*/0, /*num_ranks=*/1, {1, 1, 1}};
    JobInstance job(a, /*trace_dir=*/dir.string(), /*runtime=*/nullptr);

    RooflineCommEstimator est(kPeak, kMemBw, kLinkBw);
    // 1e7 + 2e9 + 1e10 = 12,010,000,000 ns.
    EXPECT_EQ(est.estimate(job), 12010000000ULL);
    EXPECT_EQ(est.name(), "roofline-comm");

    fs::remove_all(dir);
}

// A missing/unreadable trace is a hard error, not a silent zero: estimate()
// exits(1) so a bogus est=0 can't corrupt EASY's reservation math. The
// diagnostic goes through the spdlog logger (not the child's stderr), so we
// assert only the exit code -- with /no/such/dir the missing-trace branch is
// the only path that can exit(1). Empty regex matches any child output.
TEST(RooflineCommEstimatorDeath, HardFailsWhenTraceMissing) {
    JobArrival a{/*job_id=*/0, /*arrival=*/0, /*num_ranks=*/1, {1, 1, 1}};
    JobInstance job(a, /*trace_dir=*/"/no/such/dir", /*runtime=*/nullptr);
    RooflineCommEstimator est(kPeak, kMemBw, kLinkBw);
    EXPECT_EXIT(est.estimate(job), ::testing::ExitedWithCode(1), "");
}

}  // namespace
