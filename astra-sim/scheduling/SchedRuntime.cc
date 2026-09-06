/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/SchedRuntime.hh"

#include "astra-sim/common/Logging.hh"
#include "astra-sim/scheduling/Backfill.hh"
#include "astra-sim/scheduling/FreeNpus.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/system/Sys.hh"
#include "astra-sim/workload/Workload.hh"

#include <astra-network-analytical/common/EventQueue.h>
#include <astra-network-analytical/common/Type.h>

#include <algorithm>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <malloc.h>
#include <unistd.h>

namespace AstraSim {
namespace Scheduling {

namespace {

// FCFS order: smallest (arrival_time, job_id) first.
bool fcfs_less(const JobInstance* a, const JobInstance* b) {
    if (a->arrival_time != b->arrival_time) {
        return a->arrival_time < b->arrival_time;
    }
    return a->job_id < b->job_id;
}

// Trampoline contexts: allocated on heap, freed by the trampoline.
struct ArrivalCtx {
    SchedRuntime* runtime;
    int arrival_idx;
};

struct DetachCtx {
    SchedRuntime* runtime;
    JobInstance* job;
};

// --- CRIU checkpoint safe point ---------------------------------------------
// `ckpt.py checkpoint` sends SIGUSR1. run()'s loop notices the flag at the
// next event-bucket boundary (where the event queue is fully consistent),
// sheds droppable memory, flushes logs, and SIGSTOPs itself so `criu dump`
// captures a small, loop-aligned image. SIGCONT resumes the loop in place.
volatile std::sig_atomic_t g_checkpoint_requested = 0;

void request_checkpoint(int /*signum*/) {
    g_checkpoint_requested = 1;
}

// jemalloc's control API when the allocator is linked in (USE_JEMALLOC=ON,
// the default). Weak so the glibc-fallback build still links; the archive
// member defining mallctl is already pulled in by malloc itself.
extern "C" int mallctl(const char* name,
                       void* oldp,
                       size_t* oldlenp,
                       void* newp,
                       size_t newlen) __attribute__((weak));

// Return freed heap pages to the OS so the CRIU image shrinks. jemalloc keeps
// freed pages mapped, so without this purge a cleared route cache would not
// reduce the dump size at all.
void release_freed_memory() {
    if (mallctl != nullptr) {
        // "arena.<MALLCTL_ARENAS_ALL>.purge"; 4096 is jemalloc's stable ABI
        // constant, spelled numerically so no jemalloc header is needed.
        mallctl("arena.4096.purge", nullptr, nullptr, nullptr, 0);
    } else {
        malloc_trim(0);
    }
}

long resident_mib() {
    long pages_total = 0;
    long pages_resident = 0;
    FILE* statm = std::fopen("/proc/self/statm", "r");
    if (statm == nullptr) {
        return -1;
    }
    if (std::fscanf(statm, "%ld %ld", &pages_total, &pages_resident) != 2) {
        pages_resident = -1;
    }
    std::fclose(statm);
    if (pages_resident < 0) {
        return -1;
    }
    return pages_resident * sysconf(_SC_PAGESIZE) / (1024 * 1024);
}

void fire_arrival(void* arg) {
    auto* ctx = static_cast<ArrivalCtx*>(arg);
    ctx->runtime->on_arrival_sentinel(ctx->arrival_idx);
    delete ctx;
}

void fire_detach(void* arg) {
    auto* ctx = static_cast<DetachCtx*>(arg);
    ctx->runtime->detach_job(ctx->job);
    delete ctx;
}

}  // namespace

SchedRuntime::SchedRuntime(NetworkAnalytical::EventQueue* eq,
                           std::vector<Sys*> all_sys,
                           std::vector<int> physical_dims,
                           std::unique_ptr<PlacementPolicy> placement,
                           std::unique_ptr<AdmissionPolicy> admission,
                           std::vector<JobArrival> arrivals,
                           std::string jobs_dir,
                           JobStatsWriter* writer)
    : event_queue_(eq),
      all_sys_(std::move(all_sys)),
      physical_dims_(std::move(physical_dims)),
      placement_(std::move(placement)),
      admission_(std::move(admission)),
      arrivals_(std::move(arrivals)),
      jobs_dir_(std::move(jobs_dir)),
      writer_(writer) {}

ClusterView SchedRuntime::snapshot_cluster_view() const {
    return snapshot_cluster_view_adjusted({}, {});
}

ClusterView SchedRuntime::snapshot_cluster_view_adjusted(
    const std::unordered_set<int>& withheld,
    const std::unordered_set<int>& released) const {
    // Plain snapshots (the hot path) read busy_npus_ directly; only easyshape's
    // adjusted views pay for a copy.
    const std::unordered_set<int>* busy = &busy_npus_;
    std::unordered_set<int> adjusted;
    if (!withheld.empty() || !released.empty()) {
        adjusted = busy_npus_;
        for (int id : released) {
            adjusted.erase(id);
        }
        adjusted.insert(withheld.begin(), withheld.end());
        busy = &adjusted;
    }
    std::vector<int> free = free_npus_excluding(
        static_cast<int>(all_sys_.size()), *busy, failed_npus_);
    // Usable capacity excludes permanently-failed NPUs. Policies use
    // total_npus() only as the "job can never fit" DROP bound, so passing the
    // usable count makes oversized jobs DROP instead of DEFER forever. The
    // full physical geometry is carried separately by physical_dims().
    const int usable_npus = static_cast<int>(all_sys_.size()) -
                            static_cast<int>(failed_npus_.size());
    return ClusterView(std::move(free), physical_dims_, usable_npus,
                       static_cast<Tick>(event_queue_->get_current_time()),
                       failed_npus_);
}

SchedContext SchedRuntime::make_sched_context(
    const JobInstance* placing) const {
    SchedContext ctx;
    for (const JobInstance* j : pending_) {
        if (j != placing) {
            ++ctx.queue_depth;
        }
    }
    return ctx;
}

void SchedRuntime::schedule_all_arrival_sentinels() {
    for (int i = 0; i < static_cast<int>(arrivals_.size()); ++i) {
        auto* ctx = new ArrivalCtx{this, i};
        event_queue_->schedule_event(static_cast<NetworkAnalytical::EventTime>(
                                         arrivals_[i].arrival_time),
                                     &fire_arrival, ctx);
    }
}

void SchedRuntime::on_arrival_sentinel(int idx) {
    JobInstance* job = registry_.create(arrivals_[idx], this);
    job->trace_dir = jobs_dir_ + "/" + std::to_string(job->job_id);
    admission_->on_arrival(*job);
    if (ctx_estimator_ && !job->est_duration.has_value()) {
        job->est_duration = static_cast<Tick>(ctx_estimator_->estimate(*job));
    }
    pending_.push_back(job);
    next_arrival_idx_++;
    sweep();
}

void SchedRuntime::place_job(JobInstance* job, const std::vector<int>& npus) {
    auto logger = LoggerFactory::get_logger("scheduling");
    if (static_cast<int>(npus.size()) != job->num_ranks) {
        logger->critical("policy returned PLACED with {} npus but K={}",
                         npus.size(), job->num_ranks);
        std::exit(1);
    }
    std::unordered_set<int> unique_npus;
    for (int n : npus) {
        if (busy_npus_.find(n) != busy_npus_.end()) {
            logger->critical("policy returned a busy NPU id {}", n);
            std::exit(1);
        }
        if (failed_npus_.find(n) != failed_npus_.end()) {
            logger->critical("policy returned a failed NPU id {}", n);
            std::exit(1);
        }
        if (!unique_npus.insert(n).second) {
            // The busy/failed checks all run against the pre-placement sets,
            // so a duplicated free NPU passes them and silently overwrites
            // the first rank's Workload at attach -- the job then hangs far
            // from the root cause.
            logger->critical("policy returned duplicate NPU id {}", n);
            std::exit(1);
        }
    }

    job->rank_map = npus;
    job->execution_time = static_cast<Tick>(event_queue_->get_current_time());
    job->status = JobStatus::RUNNING;
    running_jobs_.push_back(job);
    job->rank_workloads.resize(job->num_ranks);

    for (int r = 0; r < job->num_ranks; ++r) {
        auto* sys = all_sys_[npus[r]];
        auto wl = std::make_unique<Workload>(sys, job, r);
        sys->attach_workload(wl.get());
        busy_npus_.insert(npus[r]);
        job->rank_workloads[r] = std::move(wl);
    }
    for (int r = 0; r < job->num_ranks; ++r) {
        job->rank_workloads[r]->fire();
    }

    logger->info("job {} PLACED on {} npus at tick {}", job->job_id,
                 job->num_ranks, static_cast<uint64_t>(*job->execution_time));
    writer_->stream_occupancy_row(*job->execution_time,
                                  static_cast<int>(busy_npus_.size()),
                                  static_cast<int>(all_sys_.size()));
}

void SchedRuntime::post_detach(JobInstance* job) {
    auto* ctx = new DetachCtx{this, job};
    event_queue_->schedule_event(event_queue_->get_current_time(), &fire_detach,
                                 ctx);
}

void SchedRuntime::detach_job(JobInstance* job) {
    auto logger = LoggerFactory::get_logger("scheduling");
    job->status = JobStatus::COMPLETED;
    running_jobs_.erase(
        std::remove(running_jobs_.begin(), running_jobs_.end(), job),
        running_jobs_.end());
    for (int r = 0; r < job->num_ranks; ++r) {
        all_sys_[job->rank_map[r]]->detach_workload();
        busy_npus_.erase(job->rank_map[r]);
    }
    logger->info("job {} COMPLETED at tick {} (jct={} ns)", job->job_id,
                 static_cast<uint64_t>(*job->completed_time),
                 static_cast<uint64_t>(job->jct()));
    writer_->stream_jct_row(*job);
    writer_->stream_occupancy_row(*job->completed_time,
                                  static_cast<int>(busy_npus_.size()),
                                  static_cast<int>(all_sys_.size()));

    // Release this job's per-rank Workloads now that it has finished. Each
    // Workload owns an ETFeeder (full-size node index_map + an open trace fd),
    // a Statistics object, and comm-group state. Without this, the unique_ptrs
    // live inside the JobInstance until the JobRegistry is destroyed at end of
    // run, so memory and open file descriptors grow with the number of jobs
    // ever placed -- the source of the high-load OOM (and the EMFILE crash).
    // Safe here: Workload::finish()/report() has already run, every Sys has
    // had detach_workload() null its non-owning active_workload pointer, and a
    // finished workload has no further pending events.
    job->rank_workloads.clear();

    // Tear down the job's wiring (OCS links, route overrides, matrix cells).
    // Safe here for the same reason rank_workloads.clear() is: the job is
    // fully drained, and nothing else can be riding its OCS links -- DOR
    // route computation is pure torus geometry, so those links are reachable
    // only through this job's own overrides, which die with them.
    if (reconfig_hook_ != nullptr && !job->reconfig_plan.empty()) {
        reconfig_hook_->release(job->reconfig_plan);
    }

    // NPUs were freed (and any OCS wiring released): every memoized DEFER is
    // now stale.
    ++detach_epoch_;

    sweep();
}

void SchedRuntime::remove_from_pending(JobInstance* job) {
    pending_.erase(std::remove(pending_.begin(), pending_.end(), job),
                   pending_.end());
}

void SchedRuntime::commit_placement(JobInstance* job,
                                    const PlacementResult& r) {
    remove_from_pending(job);
    job->ordered_rings = r.ordered_rings || preserve_placement_order_;
    job->relaxed = r.relaxed;
    job->relax_link_load = r.relax_link_load;
    if (reconfig_hook_ != nullptr && !r.reconfig_plan.empty()) {
        job->reconfig_plan = r.reconfig_plan;
        reconfig_hook_->apply(job->reconfig_plan);
    }
    place_job(job, r.npus);
}

PlacementResult SchedRuntime::attempt_place(JobInstance* job,
                                            const ClusterView& view,
                                            bool use_memo) {
    const bool sticky = use_memo && placement_->defer_is_shape_sticky();
    if (sticky) {
        auto it = defer_memo_.find(job->shape);
        if (it != defer_memo_.end() && it->second == detach_epoch_) {
            // This shape deferred with a free set that was a superset of (or
            // equal to) the current one; re-running the search cannot place.
            return PlacementResult{PlacementOutcome::DEFER,
                                   {},
                                   "memoized DEFER (no NPUs freed since last "
                                   "attempt)"};
        }
    }
    SchedContext ctx;
    if (placement_->wants_sched_context()) {
        ctx = make_sched_context(job);
        placement_->set_sched_context(&ctx);
    }
    auto r = placement_->try_place(*job, view);
    placement_->set_sched_context(nullptr);
    if (sticky && r.outcome == PlacementOutcome::DEFER) {
        defer_memo_[job->shape] = detach_epoch_;
    }
    return r;
}

void SchedRuntime::sweep() {
    if (admission_->uses_backfill()) {
        easy_sweep();
        return;
    }
    if (admission_->skips_on_defer()) {
        greedy_sweep();
        return;
    }
    auto logger = LoggerFactory::get_logger("scheduling");
    // One snapshot per sweep, refreshed only after a placement mutates the
    // cluster (DROP/DEFER leave it untouched).
    ClusterView view = snapshot_cluster_view();
    while (true) {
        JobInstance* job = admission_->select_next(pending_, view);
        if (job == nullptr) {
            break;
        }
        auto r = attempt_place(job, view);
        if (r.outcome == PlacementOutcome::PLACED) {
            commit_placement(job, r);
            view = snapshot_cluster_view();
        } else if (r.outcome == PlacementOutcome::DROP) {
            remove_from_pending(job);
            job->status = JobStatus::DROPPED;
            logger->warn("job {} DROPPED: {}", job->job_id, r.reason);
        } else {  // DEFER — strict-stop / head-of-line blocking
            logger->debug("job {} DEFER: {}", job->job_id, r.reason);
            break;
        }
    }
}

// Non-blocking ("packing") sweep: walk the pending set in the admission
// policy's order (ljsfpack = descending num_ranks) and admit every job that
// fits right now, SKIPPING — not blocking on — any that DEFER. With FirstFit
// placement this realizes finite-bin First-Fit-Decreasing. Selected when
// admission_->skips_on_defer() is true. One descending pass is maximal:
// admission only ever consumes free NPUs within a sweep, so a job that DEFERs
// now cannot become placeable later in the same sweep. `eligible` is pending_
// minus the jobs already attempted this sweep; a job leaves it on every outcome
// (PLACED/DROP also leave pending_; DEFER stays pending_ for a later tick but
// is not reconsidered this sweep).
void SchedRuntime::greedy_sweep() {
    auto logger = LoggerFactory::get_logger("scheduling");
    std::vector<JobInstance*> eligible(pending_.begin(), pending_.end());
    ClusterView view = snapshot_cluster_view();
    while (true) {
        JobInstance* job = admission_->select_next(eligible, view);
        if (job == nullptr) {
            break;
        }
        // Swap-and-pop: selection scans the whole vector with a strict total
        // key order, so eligible's element order is irrelevant -- no need for
        // the order-preserving O(q) erase.
        auto it = std::find(eligible.begin(), eligible.end(), job);
        *it = eligible.back();
        eligible.pop_back();
        auto r = attempt_place(job, view);
        if (r.outcome == PlacementOutcome::PLACED) {
            commit_placement(job, r);
            view = snapshot_cluster_view();
        } else if (r.outcome == PlacementOutcome::DROP) {
            remove_from_pending(job);
            job->status = JobStatus::DROPPED;
            logger->warn("job {} DROPPED: {}", job->job_id, r.reason);
        } else {  // DEFER — skip (non-blocking); stays pending for a later tick
            logger->debug("job {} DEFER (skipped): {}", job->job_id, r.reason);
        }
    }
}

void SchedRuntime::easy_sweep() {
    auto logger = LoggerFactory::get_logger("scheduling");
    const auto now = static_cast<Tick>(event_queue_->get_current_time());

    // FCFS head of the pending set: smallest (arrival_time, job_id).
    auto fcfs_head = [this]() -> JobInstance* {
        return pending_.empty() ? nullptr
                                : *std::min_element(pending_.begin(),
                                                    pending_.end(), fcfs_less);
    };

    // Place the FCFS head repeatedly until one cannot place (the pivot),
    // dropping any unplaceable head along the way.
    ClusterView view = snapshot_cluster_view();
    while (!pending_.empty()) {
        JobInstance* head = fcfs_head();
        auto r = attempt_place(head, view);
        if (r.outcome == PlacementOutcome::PLACED) {
            commit_placement(head, r);
            view = snapshot_cluster_view();
            continue;
        }
        if (r.outcome == PlacementOutcome::DROP) {
            remove_from_pending(head);
            head->status = JobStatus::DROPPED;
            logger->warn("job {} DROPPED: {}", head->job_id, r.reason);
            continue;
        }

        // DEFER: `head` is the pivot. easyshape reserves the pivot's probed
        // placement; without one (or under plain easy) fall back to the
        // count-based reservation below.
        if (admission_->shape_aware_reservation() &&
            easyshape_backfill(head, now, view)) {
            return;
        }
        // Compute the pivot's count-based reservation from currently-running
        // jobs' projected completion times, then backfill any
        // reservation-safe candidate that can place now.
        // Failed NPUs are permanently unavailable, so they reduce the usable
        // capacity that EASY's count-based reservation reasons about — exactly
        // like a smaller cluster. (No-op when no NPUs are failed.)
        const int free_now = static_cast<int>(all_sys_.size()) -
                             static_cast<int>(busy_npus_.size()) -
                             static_cast<int>(failed_npus_.size());
        // Iterate the maintained running list, not the whole registry: the
        // registry keeps every job ever created (for end-of-run stats), so a
        // full scan per DEFER is O(total-jobs x sweep-events) over a run.
        std::vector<RunningJob> running;
        running.reserve(running_jobs_.size());
        for (const JobInstance* j : running_jobs_) {
            const Tick exec = j->execution_time.value_or(now);
            const auto est = static_cast<Tick>(j->est_duration.value_or(0));
            running.push_back(
                RunningJob{std::max(now, exec + est), j->num_ranks, j->job_id});
        }
        const Reservation res = compute_reservation(
            now, head->num_ranks, free_now, std::move(running));
        if (!res.exists) {
            logger->debug(
                "job {} DEFER: no count-based reservation; skipping backfill",
                head->job_id);
            return;
        }
        int extra = res.extra;

        // Scan candidates in FCFS order. Take a sorted snapshot of pending_ so
        // the scan order is stable even as commit_placement erases entries.
        std::vector<JobInstance*> candidates = pending_;
        std::sort(candidates.begin(), candidates.end(), fcfs_less);
        for (JobInstance* cand : candidates) {
            if (cand == head) {
                continue;  // never backfill the pivot itself
            }
            const Tick cand_end =
                now + static_cast<Tick>(cand->est_duration.value_or(0));
            if (!backfill_safe(res, cand_end, cand->num_ranks, extra)) {
                continue;
            }
            auto cr = attempt_place(cand, view);
            if (cr.outcome == PlacementOutcome::PLACED) {
                commit_placement(cand, cr);
                view = snapshot_cluster_view();
                // Consume reservation slack only for a job admitted via the
                // extra-nodes path (condition b: finishes after shadow_time).
                if (cand_end > res.shadow_time) {
                    extra -= cand->num_ranks;
                }
            } else if (cr.outcome == PlacementOutcome::DROP) {
                remove_from_pending(cand);
                cand->status = JobStatus::DROPPED;
                logger->warn("job {} DROPPED: {}", cand->job_id, cr.reason);
            }
            // DEFER: candidate cannot place now; leave it pending.
        }
        return;  // pivot stays pending; this pass is done
    }
}

std::optional<ShapeReservation> SchedRuntime::probe_shape_reservation(
    JobInstance* head, Tick now) {
    // Drain order: same (projected_end, ranks, job_id) key as
    // compute_reservation, so the two reservations agree on who frees first.
    struct Ending {
        Tick end;
        const JobInstance* job;
    };
    std::vector<Ending> order;
    order.reserve(running_jobs_.size());
    for (const JobInstance* j : running_jobs_) {
        const Tick exec = j->execution_time.value_or(now);
        const auto est = static_cast<Tick>(j->est_duration.value_or(0));
        order.push_back(Ending{std::max(now, exec + est), j});
    }
    std::sort(order.begin(), order.end(), [](const Ending& a, const Ending& b) {
        if (a.end != b.end) {
            return a.end < b.end;
        }
        if (a.job->num_ranks != b.job->num_ranks) {
            return a.job->num_ranks < b.job->num_ranks;
        }
        return a.job->job_id < b.job->job_id;
    });
    // first_placeable_prefix only moves its upper bound on a true probe, so
    // the last PLACED result it saw is the placement at the k it returns.
    std::optional<PlacementResult> placed;
    const auto places = [&](int k) {
        std::unordered_set<int> released;
        for (int i = 0; i < k; ++i) {
            released.insert(order[i].job->rank_map.begin(),
                            order[i].job->rank_map.end());
        }
        auto r =
            attempt_place(head, snapshot_cluster_view_adjusted({}, released),
                          /*use_memo=*/false);
        if (r.outcome != PlacementOutcome::PLACED) {
            return false;
        }
        placed = std::move(r);
        return true;
    };
    const int k =
        first_placeable_prefix(static_cast<int>(order.size()), places);
    if (k < 0) {
        return std::nullopt;
    }
    return ShapeReservation{k == 0 ? now : order[k - 1].end,
                            std::move(placed->npus)};
}

bool SchedRuntime::easyshape_backfill(JobInstance* head,
                                      Tick now,
                                      ClusterView& view) {
    auto logger = LoggerFactory::get_logger("scheduling");
    if (shape_pivot_id_ != head->job_id) {
        shape_res_ = probe_shape_reservation(head, now);
        shape_pivot_id_ = head->job_id;
        if (shape_res_) {
            const auto [lo, hi] = std::minmax_element(shape_res_->npus.begin(),
                                                      shape_res_->npus.end());
            logger->info("job {} DEFER: shape reservation of {} NPUs [{}..{}] "
                         "shadow +{} ns (waited {} ns, {} running)",
                         head->job_id, shape_res_->npus.size(), *lo, *hi,
                         shape_res_->shadow_time - now,
                         now - head->arrival_time, running_jobs_.size());
        } else {
            logger->debug("job {} DEFER: no shape reservation; count-based "
                          "fallback",
                          head->job_id);
        }
    }
    if (!shape_res_) {
        return false;
    }
    const std::unordered_set<int> reserved(shape_res_->npus.begin(),
                                           shape_res_->npus.end());
    ClusterView view_excl = snapshot_cluster_view_adjusted(reserved, {});
    std::vector<JobInstance*> candidates = pending_;
    std::sort(candidates.begin(), candidates.end(), fcfs_less);
    for (JobInstance* cand : candidates) {
        if (cand == head) {
            continue;
        }
        const Tick cand_end =
            now + static_cast<Tick>(cand->est_duration.value_or(0));
        // (a) ends before the pivot needs its region: any NPU, plain view
        //     (memo-safe). (b) outlives the shadow time: the reserved NPUs
        //     are withheld, and the memo is bypassed since this view is not
        //     the true free set.
        auto cr = cand_end <= shape_res_->shadow_time
                      ? attempt_place(cand, view)
                      : attempt_place(cand, view_excl, /*use_memo=*/false);
        if (cr.outcome == PlacementOutcome::PLACED) {
            commit_placement(cand, cr);
            view = snapshot_cluster_view();
            view_excl = snapshot_cluster_view_adjusted(reserved, {});
        } else if (cr.outcome == PlacementOutcome::DROP) {
            remove_from_pending(cand);
            cand->status = JobStatus::DROPPED;
            logger->warn("job {} DROPPED: {}", cand->job_id, cr.reason);
        }
        // DEFER: candidate cannot place now; leave it pending.
    }
    return true;
}

bool SchedRuntime::simulation_done() const {
    return next_arrival_idx_ == static_cast<int>(arrivals_.size()) &&
           pending_.empty() && registry_.no_running_jobs();
}

void SchedRuntime::checkpoint_stop() {
    auto logger = LoggerFactory::get_logger("scheduling");
    const long rss_before = resident_mib();
    if (checkpoint_prep_) {
        checkpoint_prep_();  // drop rebuildable caches (main.cc wires which)
    }
    release_freed_memory();
    logger->info("checkpoint safe point: tick={} rss {} MiB -> {} MiB; "
                 "stopping for CRIU dump (SIGCONT resumes)",
                 event_queue_->get_current_time(), rss_before, resident_mib());
    LoggerFactory::flush_all();
    std::raise(SIGSTOP);
    logger->info("resumed from checkpoint stop");
}

void SchedRuntime::run() {
    std::signal(SIGUSR1, request_checkpoint);
    schedule_all_arrival_sentinels();
    while (true) {
        if (g_checkpoint_requested != 0) {
            g_checkpoint_requested = 0;
            checkpoint_stop();
        }
        event_queue_->proceed();
        if (event_queue_->finished()) {
            for (auto* sys : all_sys_) {
                if (sys && sys->active_workload) {
                    sys->active_workload->issue_dep_free_nodes();
                }
            }
        }
        if (event_queue_->finished() && !simulation_done()) {
            // Liveness valve (backstop). Grouped collectives are admitted
            // in-order per comm group (Workload::comm_admission_in_order),
            // which makes collective admission deadlock-free, so this should
            // never fire for them; if it does, the trace's dependency
            // structure contradicts the node-id admission order (or a
            // pg-less collective hit the legacy shared cap). At quiescence
            // nothing else can move, so re-issue once with caps and ordering
            // lifted; runs that never drain incomplete are byte-identical.
            auto logger = LoggerFactory::get_logger("scheduling");
            logger->warn("event queue drained with jobs incomplete — lifting "
                         "GPU comm cap once to break a collective-admission "
                         "deadlock (unexpected under ordered admission; check "
                         "trace/admission-order consistency)");
            for (auto* sys : all_sys_) {
                if (sys && sys->active_workload) {
                    sys->active_workload->hw_resource->comm_cap_bypass = true;
                    sys->active_workload->issue_dep_free_nodes();
                    sys->active_workload->hw_resource->comm_cap_bypass = false;
                }
            }
        }
        if (event_queue_->finished()) {
            if (!simulation_done() && drain_diagnostic_) {
                drain_diagnostic_();  // post-mortem dump before the assert
            }
            assert(simulation_done() &&
                   "event queue drained but simulation not done — "
                   "likely a collective deadlock");
            break;
        }
    }

    // Mark any still-pending jobs as DEFER_AT_EXIT.
    for (auto* job : pending_) {
        job->status = JobStatus::DEFER_AT_EXIT;
    }
    pending_.clear();

    writer_->emit_jobs_csv(registry_);
    writer_->emit_summary(registry_, admission_->name(), placement_->name(),
                          static_cast<int>(all_sys_.size()));
    writer_->emit_node_jobs(registry_, static_cast<int>(all_sys_.size()));
}

}  // namespace Scheduling
}  // namespace AstraSim
