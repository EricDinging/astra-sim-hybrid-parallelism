#ifndef __ASTRASIM_SCHEDULING_CLUSTERVIEW_HH__
#define __ASTRASIM_SCHEDULING_CLUSTERVIEW_HH__
#include "astra-sim/scheduling/Common.hh"
#include <unordered_set>
#include <vector>
namespace AstraSim {
namespace Scheduling {
class ClusterView {
  public:
    ClusterView(std::vector<int> free_npus,
                std::vector<int> physical_dims,
                int total_npus,
                Tick current_tick,
                std::unordered_set<int> failed_npus = {});
    const std::vector<int>& free_npus() const {
        return free_npus_;
    }
    // Permanently-failed NPU ids (empty when the run has no failures).
    // Already excluded from free_npus() and total_npus(); carried so
    // placement policies can reason about the degraded geometry — the
    // idle-torus DROP oracle must treat "idle" as "every usable NPU free",
    // not every physical NPU.
    const std::unordered_set<int>& failed_npus() const {
        return failed_npus_;
    }
    const std::vector<int>& physical_dims() const {
        return physical_dims_;
    }
    int total_npus() const {
        return total_npus_;
    }
    Tick current_tick() const {
        return current_tick_;
    }

  private:
    std::vector<int> free_npus_;
    std::vector<int> physical_dims_;
    int total_npus_;
    Tick current_tick_;
    std::unordered_set<int> failed_npus_;
};
}  // namespace Scheduling
}  // namespace AstraSim
#endif
