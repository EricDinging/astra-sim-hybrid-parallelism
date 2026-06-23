/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_SCHEDRUNTIME_HH__
#define __ASTRASIM_SCHEDULING_SCHEDRUNTIME_HH__

#include "astra-sim/scheduling/AdmissionPolicy.hh"
#include "astra-sim/scheduling/ClusterView.hh"
#include "astra-sim/scheduling/Common.hh"
#include "astra-sim/scheduling/DurationEstimator.hh"
#include "astra-sim/scheduling/JobRegistry.hh"
#include "astra-sim/scheduling/JobStatsWriter.hh"
#include "astra-sim/scheduling/PlacementPolicy.hh"
#include "astra-sim/scheduling/SchedContext.hh"

#include <functional>
#include <memory>
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
    // Non-blocking ("packing") sweep: admit every pending job that fits in the
    // policy's order, skipping (not blocking on) any that cannot place now.
    // Selected when admission_->skips_on_defer() is true.
    void greedy_sweep();
    void remove_from_pending(JobInstance* job);
    bool simulation_done() const;
    ClusterView snapshot_cluster_view() const;

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
    JobRegistry registry_;
    std::unordered_set<int> busy_npus_;
    std::unordered_set<int> failed_npus_;
    std::function<void()> drain_diagnostic_;  // deadlock post-mortem hook
    std::unique_ptr<DurationEstimator> ctx_estimator_;
    // main.cc sets this from the --preserve-placement-order CLI flag (which
    // defaults to true). This false is only the fallback for code paths that
    // never call the setter (e.g. unit tests that drive try_place directly).
    bool preserve_placement_order_ = false;
};

}  // namespace Scheduling
}  // namespace AstraSim

#endif
