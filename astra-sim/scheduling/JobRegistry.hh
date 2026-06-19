#ifndef __ASTRASIM_SCHEDULING_JOBREGISTRY_HH__
#define __ASTRASIM_SCHEDULING_JOBREGISTRY_HH__
#include "astra-sim/scheduling/JobInstance.hh"
#include <memory>
#include <unordered_map>
namespace AstraSim {
namespace Scheduling {
class JobRegistry {
  public:
    JobInstance* create(const JobArrival& arrival, class SchedRuntime* runtime);
    bool no_running_jobs() const;
    const std::unordered_map<int, std::unique_ptr<JobInstance>>& jobs() const {
        return by_id_;
    }

  private:
    std::unordered_map<int, std::unique_ptr<JobInstance>> by_id_;
};
}  // namespace Scheduling
}  // namespace AstraSim
#endif
