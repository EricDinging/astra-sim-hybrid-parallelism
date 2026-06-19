/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/AffinityMatrix.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/TopoMatch.hh"
#include "astra-sim/scheduling/TopoMatchSolver.hh"
#include "extern/graph_frontend/chakra/schema/protobuf/et_def.pb.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using AstraSim::Scheduling::assemble_matrix;
using AstraSim::Scheduling::build_affinity_matrix;
using AstraSim::Scheduling::ClusterView;
using AstraSim::Scheduling::find_next_wrap;
using AstraSim::Scheduling::JobArrival;
using AstraSim::Scheduling::JobInstance;
using AstraSim::Scheduling::PlacementOutcome;
using AstraSim::Scheduling::PlacementResult;
using AstraSim::Scheduling::RankComm;
using AstraSim::Scheduling::TopoMatch;
using AstraSim::Scheduling::TopoMatchSolver;

// --- helpers: write a programmatic Chakra .et fixture ---

// Write a LEB128 varint32 to an ostream (same encoding as
// ProtobufUtils::writeVarint32).
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

// Write a length-delimited protobuf message: varint32(size) + serialized bytes.
// Produces the exact framing that ProtobufUtils::readMessage expects.
void write_delimited(std::ostream& f,
                     const google::protobuf::MessageLite& msg) {
    std::string buf;
    msg.SerializeToString(&buf);
    write_varint32(f, static_cast<uint32_t>(buf.size()));
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
}

// Write one rank's .et: a GlobalMetadata header followed by the given nodes.
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
void add_int32_attr(ChakraProtoMsg::Node* n,
                    const std::string& name,
                    int32_t v) {
    auto* a = n->add_attr();
    a->set_name(name);
    a->set_int32_val(v);
}
void add_string_attr(ChakraProtoMsg::Node* n,
                     const std::string& name,
                     const std::string& v) {
    auto* a = n->add_attr();
    a->set_name(name);
    a->set_string_val(v);
}

// --- AffinityMatrix: pure helpers ---

TEST(AffinityMatrix, FindNextWrapMiddle) {
    EXPECT_EQ(find_next_wrap({0, 2, 4, 6}, 2), 4);
}

TEST(AffinityMatrix, FindNextWrapWrapsToFront) {
    EXPECT_EQ(find_next_wrap({0, 2, 4, 6}, 6), 0);
    EXPECT_EQ(find_next_wrap({0, 2, 4, 6}, 9), 0);
}

TEST(AffinityMatrix, FindNextWrapSingleMemberIsSelf) {
    EXPECT_EQ(find_next_wrap({3}, 3), 3);
}

TEST(AffinityMatrix, AssembleCollectiveRingAndSend) {
    // K=4, one process group "0" over ranks {0,1,2,3}.
    std::map<std::string, std::vector<int>> cg = {{"0", {0, 1, 2, 3}}};
    std::vector<RankComm> per_rank(4);
    // rank 0: 5 MB collective on pg "0" -> edge 0->1; 2 MB p2p send to 3.
    per_rank[0].coll_bytes["0"] = 5'000'000;
    per_rank[0].send_bytes[3] = 2'000'000;
    // rank 3: 7 MB collective on pg "0" -> wraps 3->0.
    per_rank[3].coll_bytes["0"] = 7'000'000;

    auto m = assemble_matrix(4, per_rank, cg);
    EXPECT_DOUBLE_EQ(m[0][1], 5.0);
    EXPECT_DOUBLE_EQ(m[0][3], 2.0);
    EXPECT_DOUBLE_EQ(m[3][0], 7.0);
    EXPECT_DOUBLE_EQ(m[1][0], 0.0);
}

TEST(AffinityMatrix, AssembleTruncatesPerEdgeToMB) {
    // 1.9 MB truncates to 1 MB (bytes / 1'000'000).
    std::map<std::string, std::vector<int>> cg = {{"0", {0, 1}}};
    std::vector<RankComm> per_rank(2);
    per_rank[0].coll_bytes["0"] = 1'900'000;
    auto m = assemble_matrix(2, per_rank, cg);
    EXPECT_DOUBLE_EQ(m[0][1], 1.0);
}

// --- build_affinity_matrix: end-to-end decode over a programmatic .et fixture
// ---

TEST(AffinityMatrix, BuildFromTraceFixture) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "astrasim_tm_fixture";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // comm_group.json: pg "0" spans ranks {0,1}.
    {
        std::ofstream j(dir / "comm_group.json");
        j << R"({"0": [0, 1]})";
    }

    // rank 0: COMM_COLL on pg "0", 5 MB -> edge 0->1.
    {
        ChakraProtoMsg::Node coll;
        coll.set_id(1);
        coll.set_type(ChakraProtoMsg::COMM_COLL_NODE);
        add_string_attr(&coll, "pg_name", "0");
        add_int64_attr(&coll, "comm_size", 5000000);
        write_et((dir / "chakra_trace.0.et").string(), {coll});
    }
    // rank 1: COMM_SEND to dst 0, 3 MB -> edge 1->0.
    {
        ChakraProtoMsg::Node send;
        send.set_id(1);
        send.set_type(ChakraProtoMsg::COMM_SEND_NODE);
        add_int32_attr(&send, "comm_dst", 0);
        add_int64_attr(&send, "comm_size", 3000000);
        write_et((dir / "chakra_trace.1.et").string(), {send});
    }

    auto m = build_affinity_matrix(dir.string(), 2);
    ASSERT_TRUE(m.has_value());
    EXPECT_DOUBLE_EQ((*m)[0][1], 5.0);
    EXPECT_DOUBLE_EQ((*m)[1][0], 3.0);
    EXPECT_DOUBLE_EQ((*m)[0][0], 0.0);

    // Wrong K -> nullopt.
    EXPECT_FALSE(build_affinity_matrix(dir.string(), 3).has_value());

    fs::remove_all(dir);
}

// --- TopoMatchSolver: libtopomatch wrapper ---

TEST(TopoMatchSolver, MapsRanksToDistinctFreeNodes) {
    TopoMatchSolver solver(2, 2, 2);  // 8-node torus
    std::vector<int> free_ids = {0, 1, 2, 3, 4, 5, 6, 7};
    // 4 ranks: two strongly-communicating pairs (0<->1, 2<->3).
    std::vector<std::vector<double>> aff(4, std::vector<double>(4, 0.0));
    aff[0][1] = aff[1][0] = 100.0;
    aff[2][3] = aff[3][2] = 100.0;

    auto sigma = solver.solve(free_ids, aff);
    ASSERT_TRUE(sigma.has_value());
    ASSERT_EQ(sigma->size(), 4u);
    std::set<int> allowed(free_ids.begin(), free_ids.end());
    std::set<int> used;
    for (int id : *sigma) {
        EXPECT_TRUE(allowed.count(id) == 1);  // within the binding set
        EXPECT_TRUE(used.insert(id).second);  // distinct
    }
}

TEST(TopoMatchSolver, Deterministic) {
    TopoMatchSolver s1(2, 2, 2);
    TopoMatchSolver s2(2, 2, 2);
    std::vector<int> free_ids = {0, 1, 2, 3, 4, 5, 6, 7};
    std::vector<std::vector<double>> aff(4, std::vector<double>(4, 0.0));
    aff[0][1] = aff[1][0] = 50.0;
    aff[1][2] = aff[2][1] = 25.0;
    auto a = s1.solve(free_ids, aff);
    auto b = s2.solve(free_ids, aff);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*a, *b);
}

// --- TopoMatch: placement policy ---

JobInstance tm_make_job(int id, int A, int B, int C, const std::string& dir) {
    JobArrival arr{};
    arr.job_id = id;
    arr.arrival_time = 0;
    arr.num_ranks = A * B * C;
    arr.shape = {A, B, C};
    return JobInstance(arr, dir, nullptr);
}

TEST(TopoMatch, DropsOnNonThreeDimDims) {
    TopoMatch policy;
    auto job = tm_make_job(1, 2, 1, 1, "");
    ClusterView view({0, 1}, {64}, 64, 0);
    PlacementResult r = policy.try_place(job, view);
    EXPECT_EQ(r.outcome, PlacementOutcome::DROP);
    EXPECT_EQ(r.reason,
              "topomatch requires 3-D physical_dims (--npus-per-dim)");
}

TEST(TopoMatch, DropsWhenRanksExceedTotal) {
    TopoMatch policy;
    auto job = tm_make_job(1, 4, 4, 4, "");  // 64 ranks
    ClusterView view({0, 1, 2, 3, 4, 5, 6, 7}, {2, 2, 2}, 8, 0);
    PlacementResult r = policy.try_place(job, view);
    EXPECT_EQ(r.outcome, PlacementOutcome::DROP);
    EXPECT_EQ(r.reason, "num_ranks exceeds total NPUs");
}

TEST(TopoMatch, DefersWhenTooFewFree) {
    TopoMatch policy;
    auto job = tm_make_job(1, 2, 2, 2, "");        // 8 ranks
    ClusterView view({0, 1, 2}, {2, 2, 2}, 8, 0);  // only 3 free
    PlacementResult r = policy.try_place(job, view);
    EXPECT_EQ(r.outcome, PlacementOutcome::DEFER);
    EXPECT_EQ(r.reason, "fewer free NPUs than num_ranks");
}

TEST(TopoMatch, DropsWhenTracesMissing) {
    TopoMatch policy;
    auto job = tm_make_job(1, 2, 2, 2, "/nonexistent/trace/dir");  // 8 ranks
    ClusterView view({0, 1, 2, 3, 4, 5, 6, 7}, {2, 2, 2}, 8, 0);
    PlacementResult r = policy.try_place(job, view);
    EXPECT_EQ(r.outcome, PlacementOutcome::DROP);
    EXPECT_EQ(r.reason, "cannot build affinity matrix from traces");
}

}  // namespace
