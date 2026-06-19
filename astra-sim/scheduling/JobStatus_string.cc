#include "astra-sim/scheduling/Common.hh"

namespace AstraSim {
namespace Scheduling {

std::string to_string(JobStatus s) {
    switch (s) {
    case JobStatus::PENDING:
        return "PENDING";
    case JobStatus::RUNNING:
        return "RUNNING";
    case JobStatus::COMPLETED:
        return "COMPLETED";
    case JobStatus::DROPPED:
        return "DROPPED";
    case JobStatus::DEFER_AT_EXIT:
        return "DEFER-AT-EXIT";
    }
    return "UNKNOWN";
}

}  // namespace Scheduling
}  // namespace AstraSim
