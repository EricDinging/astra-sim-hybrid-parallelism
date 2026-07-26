/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/CmdLineParser.hh"

using namespace AstraSimAnalytical;

CmdLineParser::CmdLineParser(const char* const argv0) noexcept
    : options(argv0, "ASTRA-sim") {
    parsed = {};

    // define options
    define_options();
}

void CmdLineParser::define_options() noexcept {
    // No allow_unrecognised_options(): a typo'd flag (--failure-porb=...)
    // must be a startup error, not a silent fallback to the default -- that
    // failure mode is dangerous for multi-day farm sweeps.
    options.set_width(70).add_options()("workload-configuration",
                                        "Workload configuration file",
                                        cxxopts::value<std::string>())(
        "comm-group-configuration", "Communicator group configuration file",
        cxxopts::value<std::string>()->default_value("empty"))(
        "system-configuration", "System configuration file",
        cxxopts::value<std::string>())("remote-memory-configuration",
                                       "Remote memory configuration file",
                                       cxxopts::value<std::string>())(
        "network-configuration", "Network configuration file",
        cxxopts::value<std::string>())(
        "logging-configuration", "Logging configuration file",
        cxxopts::value<std::string>()->default_value("empty"))(
        "logging-folder", "Logging folder",
        cxxopts::value<std::string>()->default_value("log"))(
        "num-queues-per-dim", "Number of queues per each dimension",
        cxxopts::value<int>()->default_value("1"))(
        "compute-scale", "Compute scale",
        cxxopts::value<double>()->default_value("1"))(
        "comm-scale", "Communication scale",
        cxxopts::value<double>()->default_value("1"))(
        "injection-scale", "Injection scale",
        cxxopts::value<double>()->default_value("1"))(
        "rendezvous-protocol", "Whether to enable rendezvous protocol",
        cxxopts::value<bool>()->default_value("false"))(
        "bw-schedule", "Per-topology bandwidth matrices file",
        cxxopts::value<std::string>())(
        "latency-schedule",
        "Per-topology latency matrices file (optional; falls back to scalar "
        "latency from network.yml)",
        cxxopts::value<std::string>()->default_value(""))(
        "npus-per-dim",
        "NPUs per dimension as comma-separated ints (e.g. 4,4,4); enables DOR "
        "routing",
        cxxopts::value<std::string>()->default_value(""))(
        "job-arrival-file", "Path to job arrivals CSV.",
        cxxopts::value<std::string>()->default_value(""))(
        "jobs-dir", "Directory containing per-job traces.",
        cxxopts::value<std::string>()->default_value(""))(
        "placement-policy",
        "firstfit | random | sfc | l1clustering | topomatch | rfold",
        cxxopts::value<std::string>()->default_value("firstfit"))(
        "admission-policy",
        "fifo | sjdf | sjsf | swf | easy | ljdf | ljsf | ljsfpack | lwf",
        cxxopts::value<std::string>()->default_value("fifo"))(
        "duration-estimator", "roofline-comm",
        cxxopts::value<std::string>()->default_value("roofline-comm"))(
        "defrag-metric", "fewest-blocks | compactness",
        cxxopts::value<std::string>()->default_value("fewest-blocks"))(
        "block-size",
        "AxBxC block tiling (rfold: scorer granularity and OCS hardware block; "
        "must divide each --npus-per-dim)",
        cxxopts::value<std::string>()->default_value("4x4x4"))(
        "rfold-selector",
        "contiguous | min-reconfig (rfold only; default min-reconfig)",
        cxxopts::value<std::string>()->default_value("min-reconfig"))(
        "rfold-ranking",
        "frag-first (default: rank by newly-fragmented cubes, no contiguity "
        "tier) | comm-first (contiguity-tiered) | packing-first | switch",
        cxxopts::value<std::string>()->default_value("frag-first"))(
        "rfold-search-budget",
        "Bounded-search expansion cap for rfold scatter selectors",
        cxxopts::value<int>()->default_value("50000"))(
        "rfold-multifold",
        "Enable multi-fold (serpentine) variant enumeration (rfold)",
        cxxopts::value<bool>()->default_value("true"))(
        "switch-theta", "DynamicSwitch backlog threshold",
        cxxopts::value<int>()->default_value("1"))(
        "failure-prob",
        "Fraction of NPUs to mark failed at startup (0..1); 0 disables. "
        "Routing "
        "detours around failed NPUs via deterministic local sidestep; rates "
        "above ~1% on large tori can exhaust local detours and abort the run. "
        "Recommended <= ~0.01.",
        cxxopts::value<double>()->default_value("0"))(
        "preserve-placement-order",
        "Build each job's collective ring in the placement policy's emitted "
        "NPU order instead of sorted-id order (rfold always does this for its "
        "own jobs). Lets locality-aware scatter policies (sfc/topomatch/"
        "l1clustering) shape the ring. Defaults to true; pass "
        "--preserve-placement-order=false to restore the legacy sorted-id "
        "ring.",
        cxxopts::value<bool>()->default_value("true"))(
        "bidi",
        "Bidirectional torus DOR: baseline routes pick the shorter arc per "
        "dimension instead of always +1 (reconfigurable backend, "
        "--npus-per-dim "
        "only). Mapping-aware policies (folding/rfold) still override this for "
        "their own ring edges. Default false (unidirectional).",
        cxxopts::value<bool>()->default_value("false"))(
        "fullmesh",
        "Treat the network as a fully connected mesh (reconfigurable backend "
        "only). The BW/latency matrices are still read and must be a "
        "single-topology schedule (topo 0) with every off-diagonal BW cell "
        "nonzero (validated at startup, then compressed and freed); routing "
        "is the direct 1-hop path (== BFS shortest path on a complete graph) "
        "and links are created lazily on first traffic. --npus-per-dim still "
        "defines the logical grid for placement policies. Incompatible with "
        "--bidi and --placement-policy=rfold; --failure-prob > 0 excludes "
        "failed NPUs from placement (direct routes never transit them).",
        cxxopts::value<bool>()->default_value("false"))(
        "route-cache-budget-gb",
        "DOR route-cache memory ceiling in GiB (reconfigurable backend with "
        "--npus-per-dim only). DOR routes are computed on demand and cached up "
        "to this bound; OCS overrides are always kept. Default 100.",
        cxxopts::value<double>()->default_value("100"))(
        "congestion-model",
        "serial (exclusive links + FIFO, the historical model) | fluid "
        "(flow-level bandwidth sharing; reconfigurable backend only)",
        cxxopts::value<std::string>()->default_value("fluid"));
}

void CmdLineParser::parse(int argc, char* argv[]) noexcept {
    try {
        // try parsing command line options
        parsed = options.parse(argc, argv);
    } catch (const cxxopts::OptionException& e) {
        // error occurred
        std::cerr << "[Error] (AstraSim/analytical/common) "
                  << "Error parsing options: " << e.what() << std::endl;
        exit(-1);
    }
}

cxxopts::Options& CmdLineParser::get_options() {
    return options;
}
