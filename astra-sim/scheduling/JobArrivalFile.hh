/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_JOBARRIVALFILE_HH__
#define __ASTRASIM_SCHEDULING_JOBARRIVALFILE_HH__

#include "astra-sim/scheduling/Common.hh"

#include <string>
#include <vector>

namespace AstraSim {
namespace Scheduling {

class JobArrivalFile {
  public:
    // Reads the CSV at `path`, validates rows, returns the parsed list
    // sorted by (arrival_time, job_id). Hard-exits on schema errors.
    static std::vector<JobArrival> parse(const std::string& path);
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
