// Placeability census probe for the 16x16x16 torus: can a placement policy
// place a given job shape on a degraded-idle cluster (every usable NPU
// free)? Optional permanent NPU failures come from the repo's own
// select_failed_npus(N, prob, seed=42), matching a production
// --failure-prob run. Drive it with run_placeability_census.sh.
//
// Modes (output: "a b c PLACED|DROP|DEFER|NOPE elapsed_ms" per shape):
//   placeability_probe <pol> shape <a> <b> <c> [fprob]
//       One shape via the policy's real try_place (full fidelity:
//       distinguishes DROP from DEFER).
//   placeability_probe <pol> tryfile <path> <shard> <nshards> [fprob]
//       try_place over every shape in <path> (one "a b c" per line),
//       processing lines with index % nshards == shard.
//   placeability_probe <pol> oraclefile <path> <shard> <nshards> [fprob]
//       RFold only. Replicates RFold::placeable_on_idle (same enumerate +
//       selector with early exit + 1-D snake fallback) instead of
//       try_place, which evaluates every fold variant to rank candidates.
//       10-50x faster and outcome-equivalent for PLACED-vs-not: try_place
//       returns PLACED iff some variant selects (or the snake closes).
//       Validated against try_place with zero disagreements on ~3.4k
//       overlapping shapes per block size (idle) and a 30-shape failure
//       sample (2026-06-12).
// pol: ff | b<N> (RFold with N^3 blocks; b16 = whole torus = pure folding)
#include "astra-sim/scheduling/BlockModel.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/FailureSelection.hh"
#include "astra-sim/scheduling/FirstFit.hh"
#include "astra-sim/scheduling/FoldEnumerator.hh"
#include "astra-sim/scheduling/FootprintRouter.hh"
#include "astra-sim/scheduling/FragmentationScorer.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/PlacementPolicy.hh"
#include "astra-sim/scheduling/PlacementRanker.hh"
#include "astra-sim/scheduling/RFold.hh"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace AstraSim::Scheduling;

namespace {

constexpr int kD = 16;
constexpr int kN = kD * kD * kD;
constexpr unsigned kFailSeed = 42;    // production default
constexpr int kSnakeBudget = 200000;  // mirrors RFold.cc

std::unique_ptr<PlacementPolicy> make_policy(const std::string& name) {
    if (name == "ff") {
        return std::make_unique<FirstFit>();
    }
    int b = std::atoi(name.c_str() + 1);
    std::array<int, 3> blk{b, b, b};
    return std::make_unique<RFold>(
        blk, make_block_selector("min-reconfig"),
        make_fragmentation_scorer("fewest-blocks-ocs", blk),
        make_placement_ranker("comm-first"));
}

const char* outcome_str(PlacementOutcome o) {
    switch (o) {
    case PlacementOutcome::PLACED:
        return "PLACED";
    case PlacementOutcome::DEFER:
        return "DEFER";
    case PlacementOutcome::DROP:
        return "DROP";
    }
    return "?";
}

struct Cluster {
    std::unordered_set<int> failed;
    std::vector<int> usable;  // ascending, excludes failed
    std::unordered_set<int> usable_set;
};

Cluster make_cluster(double fprob) {
    Cluster c;
    auto ids = select_failed_npus(kN, fprob, kFailSeed);
    c.failed.insert(ids.begin(), ids.end());
    c.usable.reserve(kN - ids.size());
    for (int i = 0; i < kN; ++i) {
        if (c.failed.find(i) == c.failed.end()) {
            c.usable.push_back(i);
        }
    }
    c.usable_set.insert(c.usable.begin(), c.usable.end());
    return c;
}

std::vector<std::array<int, 3>> read_shapes(const char* path) {
    std::vector<std::array<int, 3>> list;
    FILE* fp = std::fopen(path, "r");
    if (!fp) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    int a, b, c;
    while (std::fscanf(fp, "%d %d %d", &a, &b, &c) == 3) {
        list.push_back({a, b, c});
    }
    std::fclose(fp);
    return list;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "see header comment for usage\n");
        return 1;
    }
    std::string pol = argv[1];
    std::string mode = argv[2];

    if (mode == "shape" && (argc == 6 || argc == 7)) {
        double fprob = argc == 7 ? std::atof(argv[6]) : 0.0;
        Cluster cl = make_cluster(fprob);
        auto policy = make_policy(pol);
        long a = std::atol(argv[3]), b = std::atol(argv[4]),
             c = std::atol(argv[5]);
        ClusterView view(std::vector<int>(cl.usable), {kD, kD, kD},
                         static_cast<int>(cl.usable.size()), 0, cl.failed);
        std::array<int, 3> s{static_cast<int>(a), static_cast<int>(b),
                             static_cast<int>(c)};
        JobArrival arr{1, 0, static_cast<int>(a * b * c), s};
        JobInstance j(arr, "", nullptr);
        auto t0 = std::chrono::steady_clock::now();
        auto r = policy->try_place(j, view);
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        std::printf("%ld %ld %ld %s %.1f\n", a, b, c, outcome_str(r.outcome),
                    ms);
        return 0;
    }

    if (mode == "tryfile" && (argc == 6 || argc == 7)) {
        long shard = std::atol(argv[4]);
        long nshards = std::atol(argv[5]);
        double fprob = argc == 7 ? std::atof(argv[6]) : 0.0;
        Cluster cl = make_cluster(fprob);
        auto policy = make_policy(pol);
        auto list = read_shapes(argv[3]);
        long idx = 0;
        int jid = 0;
        for (const auto& s : list) {
            if (idx++ % nshards != shard) {
                continue;
            }
            ClusterView view(std::vector<int>(cl.usable), {kD, kD, kD},
                             static_cast<int>(cl.usable.size()), 0, cl.failed);
            JobArrival arr{++jid, 0, s[0] * s[1] * s[2], s};
            JobInstance j(arr, "", nullptr);
            auto t0 = std::chrono::steady_clock::now();
            auto r = policy->try_place(j, view);
            double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
            std::printf("%d %d %d %s %.1f\n", s[0], s[1], s[2],
                        outcome_str(r.outcome), ms);
            std::fflush(stdout);
        }
        return 0;
    }

    if (mode == "oraclefile" && (argc == 6 || argc == 7)) {
        // Replicates RFold::placeable_on_idle on the degraded torus: same
        // enumerate + selector early-exit + 1-D snake fallback, idle set =
        // usable (non-failed) NPUs. Outcome-equivalent to try_place on a
        // degraded-idle view for PLACED-vs-not.
        long shard = std::atol(argv[4]);
        long nshards = std::atol(argv[5]);
        double fprob = argc == 7 ? std::atof(argv[6]) : 0.0;
        Cluster cl = make_cluster(fprob);
        int b = std::atoi(pol.c_str() + 1);
        std::array<int, 3> blk{b, b, b};
        auto selector = make_block_selector("min-reconfig");
        auto scorer = make_fragmentation_scorer("fewest-blocks-ocs", blk);
        auto ranker = make_placement_ranker("comm-first");
        std::vector<int> dims{kD, kD, kD};
        BlockModel cm({kD, kD, kD}, blk);
        auto list = read_shapes(argv[3]);
        const int usable_n = static_cast<int>(cl.usable.size());
        long idx = 0;
        for (const auto& s : list) {
            if (idx++ % nshards != shard) {
                continue;
            }
            const int K = s[0] * s[1] * s[2];
            auto t0 = std::chrono::steady_clock::now();
            bool ok = false;
            if (K <= usable_n) {
                const auto ring = FootprintRouter::ring_edges(s);
                for (const auto& v : FoldEnumerator::enumerate(s, true)) {
                    if (selector->select(v, cl.usable_set, dims, cm, *scorer,
                                         *ranker, ring)) {
                        ok = true;
                        break;
                    }
                }
                const int non_unit = (s[0] > 1) + (s[1] > 1) + (s[2] > 1);
                if (!ok && non_unit <= 1) {
                    ok = !FoldEnumerator::snake_1d(cl.usable, dims, K,
                                                   kSnakeBudget)
                              .empty();
                }
            }
            double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
            std::printf("%d %d %d %s %.1f\n", s[0], s[1], s[2],
                        ok ? "PLACED" : "NOPE", ms);
            std::fflush(stdout);
        }
        return 0;
    }

    std::fprintf(stderr, "bad arguments; see header comment\n");
    return 1;
}
