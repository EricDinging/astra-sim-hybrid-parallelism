/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_BACKFILL_HH__
#define __ASTRASIM_SCHEDULING_BACKFILL_HH__

#include "astra-sim/common/Common.hh"

#include <vector>

namespace AstraSim {
namespace Scheduling {

// A currently-running job, summarized for reservation arithmetic.
struct RunningJob {
    Tick projected_end;  // absolute tick the job is expected to finish (caller
                         // computes max(now, execution_time + est_duration))
    int ranks;
};

// The pivot's (FCFS head's) reservation under count-based aggressive
// backfilling. exists == false means no future time frees enough NPUs for the
// pivot (defensive; a well-formed DEFER never produces this).
struct Reservation {
    bool exists;
    Tick shadow_time;  // earliest time >= now with >= k_head NPUs free
    int extra;         // initial NPUs free at shadow_time beyond the pivot's
                       // need (the caller's starting slack; see backfill_safe)
};

// Earliest time at which >= k_head NPUs are free, counting free_now plus
// running jobs draining in projected-end order. `running` is taken by value and
// sorted internally; tie order among equal projected_end does not change the
// result. If free_now >= k_head already (the head DEFERed on torus shape, not
// capacity), shadow_time == now and extra == free_now - k_head.
Reservation compute_reservation(Tick now,
                                int k_head,
                                int free_now,
                                std::vector<RunningJob> running);

// Pure aggressive-backfill safety predicate for one candidate started now and
// finishing at cand_end, using cand_ranks NPUs. Safe iff either (a)
// cand_end <= res.shadow_time (finishes before the pivot needs nodes), or (b)
// cand_ranks <= extra (fits in nodes the pivot will not need).
//
// `extra` is the caller's CURRENT remaining slack, not res.extra: the caller
// initializes it from res.extra and decrements it by cand_ranks after each
// confirmed condition-(b) placement, then passes the live value here. This
// function only reads it (res.extra is intentionally not re-read). Returns
// false when res.exists is false.
bool backfill_safe(const Reservation& res,
                   Tick cand_end,
                   int cand_ranks,
                   int extra);

}  // namespace Scheduling
}  // namespace AstraSim

#endif
