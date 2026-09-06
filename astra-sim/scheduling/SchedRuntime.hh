/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_SCHEDRUNTIME_HH__
#define __ASTRASIM_SCHEDULING_SCHEDRUNTIME_HH__

#include "astra-sim/scheduling/AdmissionPolicy.hh"
#include "astra-sim/scheduling/Backfill.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"
#include "astra-sim/scheduling/JobRegistry.hh"
#include "astra-sim/scheduling/JobStatsWriter.hh"
#include "astra-sim/scheduling/PlacementPolicy.hh"
#include "astra-sim/scheduling/SchedContext.hh"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace NetworkAnalytical {
class EventQueue;
}

namespace AstraSim {
class Sys;

namespace Scheduling {

class SchedRuntime {
  public:
    SchedRuntime(NetworkAnalytical::EventQueue* event_queue,
                 std::vector<Sys*> all_sys,
                 std::vector<int> physical_dims,
                 std::unique_ptr<PlacementPolicy> placement,
                 std::unique_ptr<AdmissionPolicy> admission,
                 std::vector<JobArrival> arrivals,
                 std::string jobs_dir,
                 JobStatsWriter* writer);

    // Main loop: schedules arrival sentinels, drains the event queue,
    // emits stats on exit.
    void run();

    // Called by JobInstance::on_rank_finished once all K ranks report done.
    void post_detach(JobInstance* job);

    // Public so trampolines can call them.
    void on_arrival_sentinel(int arrival_idx);
    void detach_job(JobInstance* job);

    // Optional reconfiguration hook (rfold). When set, SchedRuntime applies a
    // job's ReconfigPlan via this hook on PLACED, before firing its workloads.
    void set_reconfig_hook(ReconfigHook* hook) {
        reconfig_hook_ = hook;
    }

    // Optional post-mortem hook: invoked when the event queue drains while
    // the simulation is incomplete (i.e., the liveness valve did not help),
    // just before the deadlock assert fires. The reconfigurable frontend uses
    // it to dump pending chunks, unresolved send/recv matches, and per-Sys
    // stream state.
    void set_drain_diagnostic(std::function<void()> diag) {
        drain_diagnostic_ = std::move(diag);
    }

    // Mark NPUs as permanently failed: excluded from every placement snapshot.
    // Call once before run(); empty by default.
    void set_failed_npus(const std::unordered_set<int>& failed) {
        failed_npus_ = failed;
    }

    // Drop rebuildable caches before a CRIU dump; invoked at the SIGUSR1
    // safe point (see checkpoint_stop() in SchedRuntime.cc).
    void set_checkpoint_prep(std::function<void()> prep) {
        checkpoint_prep_ = std::move(prep);
    }

    // Optional estimator used ONLY to fill SchedContext/est_duration for
    // context-aware placement rankings; independent of admission policies.
    void set_ctx_estimator(std::unique_ptr<DurationEstimator> est) {
        ctx_estimator_ = std::move(est);
    }

    // When true, build every placed job's collective ring in the placement
    // policy's emitted NPU (rank_map) order instead of sorted-id order. rfold
    // already forces ordered rings per-job via PlacementResult; this lifts the
    // same behavior to any policy (e.g. sfc/topomatch/l1clustering), so their
    // spatial locality reaches the ring. The --preserve-placement-order CLI
    // flag defaults to true, so placement-order rings are the default; pass
    // =false to restore the legacy sorted-id ring.
    void set_preserve_placement_order(bool b) {
        preserve_placement_order_ = b;
    }

  private:
    SchedContext make_sched_context(const JobInstance* placing) const;
    void schedule_all_arrival_sentinels();
    void place_job(JobInstance* job, const std::vector<int>& npus);
    void sweep();
    // Shared commit sequence for a PLACED job: remove from pending, copy
    // ordered_rings, apply any reconfig plan, then fire the workloads. Used by
    // both the default sweep loop and easy_sweep so commit semantics live in
    // one place.
    void commit_placement(JobInstance* job, const PlacementResult& r);
    // Backfilling sweep (EASY). Places the FCFS head repeatedly until one job
    // cannot place (the pivot), then backfills any reservation-safe candidate
    // that can place now. Selected when admission_->uses_backfill() is true.
    void easy_sweep();
    // Shape-aware half of easy_sweep (easyshape): reserve the pivot's probed
    // placement and backfill around it. Returns false when no shape
    // reservation exists, so easy_sweep falls back to the count-based rule.
    bool easyshape_backfill(JobInstance* head, Tick now, ClusterView& view);
    // Probe the placement policy against the current free set plus the k
    // earliest-ending running jobs, for the smallest k that places `head`.
    std::optional<ShapeReservation> probe_shape_reservation(JobInstance* head,
                                                            Tick now);
    // Non-blocking ("packing") sweep: admit every pending job that fits in the
    // policy's order, skipping (not blocking on) any that cannot place now.
    // Selected when admission_->skips_on_defer() is true.
    void greedy_sweep();
    void remove_from_pending(JobInstance* job);
    bool simulation_done() const;
    ClusterView snapshot_cluster_view() const;
    // snapshot_cluster_view with `withheld` NPUs also removed from the free
    // set (easyshape's reserved region; a probe's drained prefix passes the
    // complement via `released`, which re-adds busy NPUs).
    ClusterView snapshot_cluster_view_adjusted(
        const std::unordered_set<int>& withheld,
        const std::unordered_set<int>& released) const;
    // One placement attempt for `job`: installs/clears the SchedContext
    // around placement_->try_place. When the policy declares
    // defer_is_shape_sticky(), a DEFER outcome is memoized per shape and
    // returned without re-running the search until the next completion frees
    // NPUs (the free set only grows at detach, and a same-or-smaller free
    // set cannot flip DEFER to PLACED). All sweeps route through this.
    // `view` is the sweep-maintained cluster snapshot: sweeps build it once
    // and refresh it only after commit_placement (the only in-sweep cluster
    // mutation), instead of rebuilding the O(N) snapshot per attempt.
    // `use_memo` = false bypasses the DEFER memo both ways (neither consults
    // nor records it): required whenever `view` is not the plain cluster
    // snapshot (easyshape probes and reserved-region views), since the memo is
    // only sound against the true free set.
    PlacementResult attempt_place(JobInstance* job,
                                  const ClusterView& view,
                                  bool use_memo = true);
    // SIGUSR1 safe point; see the checkpoint block in SchedRuntime.cc.
    void checkpoint_stop();

    NetworkAnalytical::EventQueue* event_queue_;
    std::vector<Sys*> all_sys_;
    std::vector<int> physical_dims_;
    std::unique_ptr<PlacementPolicy> placement_;
    std::unique_ptr<AdmissionPolicy> admission_;
    std::vector<JobArrival> arrivals_;
    std::string jobs_dir_;
    JobStatsWriter* writer_;
    ReconfigHook* reconfig_hook_ = nullptr;

    int next_arrival_idx_ = 0;
    std::vector<JobInstance*> pending_;
    // Jobs currently RUNNING (registry-owned pointers), maintained by
    // place_job/detach_job so reservation sweeps don't scan the ever-growing
    // registry.
    std::vector<JobInstance*> running_jobs_;
    JobRegistry registry_;
    std::unordered_set<int> busy_npus_;
    std::unordered_set<int> failed_npus_;
    // DEFER memo (see attempt_place): shape -> the detach epoch at which it
    // last deferred. Bumped in detach_job -- the only event that frees NPUs
    // (the failed set is fixed at startup) -- which invalidates every entry.
    std::map<std::array<int, 3>, std::uint64_t> defer_memo_;
    std::uint64_t detach_epoch_ = 0;
    // easyshape reservation, held for the pivot's whole wait: every backfill
    // admitted after the probe either ends by the shadow time or avoids the
    // reserved NPUs, so the region drains monotonically and the pivot is
    // guaranteed to place there (or somewhere earlier) without re-probing.
    // Re-probing per detach would let the policy pick a different region
    // each time, forfeiting the protection accumulated so far. nullopt with
    // a matching pivot means "no shape reservation for this pivot".
    int shape_pivot_id_ = -1;
    std::optional<ShapeReservation> shape_res_;
    std::function<void()> drain_diagnostic_;  // deadlock post-mortem hook
    std::function<void()> checkpoint_prep_;   // SIGUSR1 checkpoint-prep hook
    std::unique_ptr<DurationEstimator> ctx_estimator_;
    // main.cc sets this from the --preserve-placement-order CLI flag (which
    // defaults to true). This false is only the fallback for code paths that
    // never call the setter (e.g. unit tests that drive try_place directly).
    bool preserve_placement_order_ = false;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
