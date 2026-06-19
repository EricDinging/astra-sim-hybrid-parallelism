#include "astra-sim/scheduling/ClusterView.hh"
namespace AstraSim {
namespace Scheduling {
ClusterView::ClusterView(std::vector<int> f,
                         std::vector<int> d,
                         int t,
                         Tick c,
                         std::unordered_set<int> failed)
    : free_npus_(std::move(f)),
      physical_dims_(std::move(d)),
      total_npus_(t),
      current_tick_(c),
      failed_npus_(std::move(failed)) {}
}  // namespace Scheduling
}  // namespace AstraSim
