/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#ifndef __ASTRASIM_SCHEDULING_SCHEDCONTEXT_HH__
#define __ASTRASIM_SCHEDULING_SCHEDCONTEXT_HH__

namespace AstraSim {
namespace Scheduling {

// Queue state for state-aware rankers (switch). Built fresh by SchedRuntime
// before each try_place; RFold installs it on its ranker for the duration of
// the call only.
struct SchedContext {
    int queue_depth = 0;  // pending jobs EXCLUDING the placing one
};

}  // namespace Scheduling
}  // namespace AstraSim
#endif
