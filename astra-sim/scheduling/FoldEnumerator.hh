/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_FOLDENUMERATOR_HH__
#define __ASTRASIM_SCHEDULING_FOLDENUMERATOR_HH__

#include <array>
#include <vector>

namespace AstraSim {
namespace Scheduling {

struct FoldVariant {
    std::array<int, 3> footprint;               // f0*f1*f2 == product(shape)
    std::vector<std::array<int, 3>> embedding;  // embedding[rank] -> (x,y,z)
    // True unless a folded (multi-axis) ring is left open. Single-axis rings
    // are neutral (their closure is a placement-time, full-axis property).
    bool ring_closes = false;
    // True for the unfolded variant (footprint == requested shape). Marks
    // the variant for tests/diagnostics; comm-first does not rank identity
    // as its own class (it competes inside full-fidelity on the packing
    // keys), but the flag stays cheap and useful.
    bool identity = false;
};

// Reflected mixed-radix (boustrophedon) coordinates of value v over the given
// axis sizes (innermost first). Digit k reflects iff the number formed by the
// higher digits is odd, so consecutive v differ by exactly 1 on exactly one
// axis at ANY tuple depth. End-to-start gap: with an even outermost size every
// inner digit returns to 0, leaving a single-axis gap of (outermost-1) — so
// the cycle closes natively iff the outermost size is 2; an even outermost
// > 2 leaves an OCS-closable single-axis gap; an odd outermost leaves a
// multi-axis gap (open).
std::vector<int> snake_coords(int v, const std::vector<int>& sizes);

// L1 distance between the boustrophedon end cell and the start cell for the
// factor tuple `sizes` (inner first). 1 => the ring closes structurally.
int snake_end_gap(const std::vector<int>& sizes);

// All ordered factor tuples (f_1 .. f_m), 1 <= m <= max_parts, every factor
// >= 2, product == n, INNER factor first (the same order Plan::dim_factor
// uses). n must be >= 2.
std::vector<std::vector<int>> ordered_factorizations(int n, int max_parts);

class FoldEnumerator {
  public:
    // Identity + hairpin/joint folds (grow) and, when `multifold`, exclusive
    // multi-factor serpentine plans (each dim's ordered factor tuple on its
    // own axes — the spare-axes rule). Deterministic; deduplicated by
    // footprint keeping the best comm class; capped at 512 variants.
    // Returns a reference into an internal memo cache (std::map, so entries
    // stay pointer-stable across later inserts). Callers must treat it as
    // read-only and must not retain it beyond the current scope.
    static const std::vector<FoldVariant>& enumerate(
        const std::array<int, 3>& shape, bool multifold = true);

    // Bounded-depth search for a closed cycle of length `len` through the
    // `free` NPU ids on torus `dims`, where consecutive ranks (and the wrap)
    // are 1 torus hop apart. Returns the ordered cycle (rank -> NPU id), or
    // empty if none is found within `max_depth` DFS expansions. `dims` must
    // have 3 elements. For 1-D jobs only: a single ring can snake through
    // fragmented free space.
    static std::vector<int> snake_1d(const std::vector<int>& free,
                                     const std::vector<int>& dims,
                                     int len,
                                     int max_depth);
};

}  // namespace Scheduling
}  // namespace AstraSim
#endif
