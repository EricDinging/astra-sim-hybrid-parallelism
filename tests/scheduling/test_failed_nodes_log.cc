/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/JobStatsWriter.hh"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using AstraSim::Scheduling::JobStatsWriter;

namespace {
std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

TEST(FailedNodesLog, WritesHeaderAndIds) {
    std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) /
                                "failed_nodes_log_WritesHeaderAndIds";
    std::filesystem::create_directories(dir);
    JobStatsWriter writer(dir.string());
    writer.emit_failed_nodes({3, 17, 102});
    EXPECT_EQ(read_file(dir.string() + "/failed_nodes.log"),
              "failed_npu_id\n3\n17\n102\n");
}

TEST(FailedNodesLog, EmptyIsHeaderOnly) {
    std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) /
                                "failed_nodes_log_EmptyIsHeaderOnly";
    std::filesystem::create_directories(dir);
    JobStatsWriter writer(dir.string());
    writer.emit_failed_nodes({});
    EXPECT_EQ(read_file(dir.string() + "/failed_nodes.log"), "failed_npu_id\n");
}
