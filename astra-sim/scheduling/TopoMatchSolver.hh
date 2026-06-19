/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_TOPOMATCHSOLVER_HH__
#define __ASTRASIM_SCHEDULING_TOPOMATCHSOLVER_HH__

#include <optional>
#include <string>
#include <vector>

namespace AstraSim {
namespace Scheduling {

// Thin C++ wrapper over libtopomatch. The only TU that includes <topomatch.h>.
// Writes the one-line `torus3D W L H` arch file once and runs TopoMatch in a
// deterministic single-threaded mode.
class TopoMatchSolver {
  public:
    TopoMatchSolver(int W, int L, int H);
    ~TopoMatchSolver();

    // free_ids: candidate global NPU ids (size >= K). affinity: K x K.
    // Returns sigma (size K, sigma[i] = global NPU id for job-local rank i),
    // or nullopt on failure / invalid solution.
    std::optional<std::vector<int>> solve(
        const std::vector<int>& free_ids,
        const std::vector<std::vector<double>>& affinity);

  private:
    int W_, L_, H_;
    std::string arch_path_;
    bool arch_written_;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
