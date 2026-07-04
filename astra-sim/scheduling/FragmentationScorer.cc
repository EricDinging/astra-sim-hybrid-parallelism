/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/scheduling/FragmentationScorer.hh"

#include "astra-sim/scheduling/Common.hh"

#include <algorithm>
#include <set>
#include <sstream>

namespace AstraSim {
namespace Scheduling {

namespace {

// Number of distinct block-grid cells the placement's NPUs intersect.
size_t count_blocks(const std::vector<int>& npus,
                    const std::vector<int>& d,
                    const std::array<int, 3>& block) {
    std::set<std::array<int, 3>> blocks;
    for (int n : npus) {
        const auto c = coord_of(n, d[0], d[1]);
        blocks.insert({c[0] / block[0], c[1] / block[1], c[2] / block[2]});
    }
    return blocks.size();
}

}  // namespace

double FewestBlocksTouched::cost(const ScoredPlacement& p) const {
    const auto& d = *p.dims;
    // Pack (blocks_touched, max_footprint_dim) into one cost where the block
    // count strictly dominates. The tiebreak multiplier is the largest torus
    // axis + 1; a footprint dim can never exceed the torus, so the tiebreak
    // can never bleed into the block-count term.
    const int max_footprint =
        std::max({p.footprint[0], p.footprint[1], p.footprint[2]});
    const int mult = std::max({d[0], d[1], d[2]}) + 1;
    return static_cast<double>(count_blocks(*p.npus, d, block_)) * mult +
           max_footprint;
}

double Compactness::cost(const ScoredPlacement& p) const {
    return std::max({p.footprint[0], p.footprint[1], p.footprint[2]});
}

double FewestBlocksThenOcsLinks::cost(const ScoredPlacement& p) const {
    const auto& d = *p.dims;
    // Pack (blocks, ocs_links, max_footprint) so each term strictly dominates
    // the next. max_footprint < wf; ocs_links <= 3*N < wo; so ocs*wf + mf can
    // never reach wo*wf (one block unit), and mf can never reach wf (one OCS
    // unit). N = total torus nodes.
    const long N = static_cast<long>(d[0]) * d[1] * d[2];
    const int mf = std::max({p.footprint[0], p.footprint[1], p.footprint[2]});
    const double wf = std::max({d[0], d[1], d[2]}) + 1.0;
    const double wo = 3.0 * static_cast<double>(N) + 1.0;
    return static_cast<double>(count_blocks(*p.npus, d, block_)) * (wo * wf) +
           static_cast<double>(p.ocs_links) * wf + mf;
}

bool parse_block_size(const std::string& s, std::array<int, 3>& out) {
    std::stringstream ss(s);
    std::string tok;
    int i = 0;
    while (std::getline(ss, tok, 'x')) {
        if (i >= 3 || tok.empty()) {
            return false;
        }
        try {
            size_t pos = 0;
            int v = std::stoi(tok, &pos);
            if (pos != tok.size() || v <= 0) {
                return false;
            }
            out[i++] = v;
        } catch (...) {
            return false;
        }
    }
    return i == 3;
}

std::unique_ptr<FragmentationScorer> make_fragmentation_scorer(
    const std::string& name, std::array<int, 3> block) {
    if (name == "fewest-blocks") {
        return std::make_unique<FewestBlocksTouched>(block);
    }
    if (name == "compactness") {
        return std::make_unique<Compactness>();
    }
    if (name == "fewest-blocks-ocs") {
        return std::make_unique<FewestBlocksThenOcsLinks>(block);
    }
    return nullptr;
}

}  // namespace Scheduling
}  // namespace AstraSim
