/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/Backfill.hh"

#include <algorithm>

namespace AstraSim {
namespace Scheduling {

Reservation compute_reservation(Tick now,
                                int k_head,
                                int free_now,
                                std::vector<RunningJob> running) {
    // The head already fits by count (it DEFERed on torus shape, not capacity):
    // reserve at `now` with whatever surplus exists.
    if (free_now >= k_head) {
        return Reservation{/*exists=*/true, /*shadow_time=*/now,
                           /*extra=*/free_now - k_head};
    }
    // Drain running jobs in projected-end order. Tie order among equal
    // projected_end does not affect shadow_time or extra (both depend only on
    // the cumulative free count at the crossing).
    std::sort(running.begin(), running.end(),
              [](const RunningJob& a, const RunningJob& b) {
                  return a.projected_end < b.projected_end;
              });
    int avail = free_now;
    for (const RunningJob& job : running) {
        avail += job.ranks;
        if (avail >= k_head) {
            return Reservation{/*exists=*/true,
                               /*shadow_time=*/job.projected_end,
                               /*extra=*/avail - k_head};
        }
    }
    return Reservation{/*exists=*/false, /*shadow_time=*/0, /*extra=*/0};
}

bool backfill_safe(const Reservation& res,
                   Tick cand_end,
                   int cand_ranks,
                   int extra) {
    if (!res.exists) {
        return false;
    }
    if (cand_end <= res.shadow_time) {
        return true;  // (a) finishes before the pivot needs nodes
    }
    return cand_ranks <= extra;  // (b) fits in nodes the pivot will not need
}

}  // namespace Scheduling
}  // namespace AstraSim
