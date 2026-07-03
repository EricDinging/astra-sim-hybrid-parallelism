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
#include <cstdlib>

namespace AstraSim {
namespace Scheduling {

namespace {

// Trampoline contexts: allocated on heap, freed by the trampoline.
struct ArrivalCtx {
    SchedRuntime* runtime;
    int arrival_idx;
};

struct DetachCtx {
    SchedRuntime* runtime;
    JobInstance* job;
};

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
    std::vector<int> free = free_npus_excluding(
        static_cast<int>(all_sys_.size()), busy_npus_, failed_npus_);
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

// Deliberate choice: `queued` is FCFS-ordered (arrival, then id) regardless
// of the admission policy. The cost-model proxy reads only depth + mean
// service time (order-independent); the lookahead arm probes the FCFS head,
// which approximates "the jobs that have waited longest" rather than the
// admission policy's next picks. Revisit if cost-model is ever swept against
// non-FCFS admission (sjdf/swf/lwf) and the lookahead arm matters there.
SchedContext SchedRuntime::make_sched_context(
    const JobInstance* placing) const {
    SchedContext ctx;
    std::vector<JobInstance*> q = pending_;
    std::sort(q.begin(), q.end(),
              [](const JobInstance* a, const JobInstance* b) {
                  if (a->arrival_time != b->arrival_time) {
                      return a->arrival_time < b->arrival_time;
                  }
                  return a->job_id < b->job_id;
              });
    for (const JobInstance* j : q) {
        if (j == placing) {
            continue;
        }
        ctx.queued.push_back(
            {j->shape, j->num_ranks,
             static_cast<double>(j->est_duration.value_or(0)) / 1e9});
    }
    ctx.queue_depth = static_cast<int>(ctx.queued.size());
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
    for (int n : npus) {
        if (busy_npus_.find(n) != busy_npus_.end()) {
            logger->critical("policy returned a busy NPU id {}", n);
            std::exit(1);
        }
        if (failed_npus_.find(n) != failed_npus_.end()) {
            logger->critical("policy returned a failed NPU id {}", n);
            std::exit(1);
        }
    }

    job->rank_map = npus;
    job->execution_time = static_cast<Tick>(event_queue_->get_current_time());
    job->status = JobStatus::RUNNING;
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
}

void SchedRuntime::post_detach(JobInstance* job) {
    auto* ctx = new DetachCtx{this, job};
    event_queue_->schedule_event(event_queue_->get_current_time(), &fire_detach,
                                 ctx);
}

void SchedRuntime::detach_job(JobInstance* job) {
    auto logger = LoggerFactory::get_logger("scheduling");
    job->status = JobStatus::COMPLETED;
    for (int r = 0; r < job->num_ranks; ++r) {
        all_sys_[job->rank_map[r]]->detach_workload();
        busy_npus_.erase(job->rank_map[r]);
    }
    logger->info("job {} COMPLETED at tick {} (jct={} ns)", job->job_id,
                 static_cast<uint64_t>(*job->completed_time),
                 static_cast<uint64_t>(job->jct()));

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
    if (reconfig_hook_ != nullptr && !r.reconfig_plan.empty()) {
        reconfig_hook_->apply(r.reconfig_plan);
    }
    place_job(job, r.npus);
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
    while (true) {
        JobInstance* job =
            admission_->select_next(pending_, snapshot_cluster_view());
        if (job == nullptr) {
            break;
        }
        auto ctx = make_sched_context(job);
        placement_->set_sched_context(&ctx);
        auto r = placement_->try_place(*job, snapshot_cluster_view());
        placement_->set_sched_context(nullptr);
        if (r.outcome == PlacementOutcome::PLACED) {
            commit_placement(job, r);
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
    while (true) {
        JobInstance* job =
            admission_->select_next(eligible, snapshot_cluster_view());
        if (job == nullptr) {
            break;
        }
        eligible.erase(std::find(eligible.begin(), eligible.end(), job));
        auto ctx = make_sched_context(job);
        placement_->set_sched_context(&ctx);
        auto r = placement_->try_place(*job, snapshot_cluster_view());
        placement_->set_sched_context(nullptr);
        if (r.outcome == PlacementOutcome::PLACED) {
            commit_placement(job, r);
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
        JobInstance* head = nullptr;
        for (JobInstance* j : pending_) {
            if (head == nullptr || j->arrival_time < head->arrival_time ||
                (j->arrival_time == head->arrival_time &&
                 j->job_id < head->job_id)) {
                head = j;
            }
        }
        return head;
    };

    // Place the FCFS head repeatedly until one cannot place (the pivot),
    // dropping any unplaceable head along the way.
    while (!pending_.empty()) {
        JobInstance* head = fcfs_head();
        auto ctx = make_sched_context(head);
        placement_->set_sched_context(&ctx);
        auto r = placement_->try_place(*head, snapshot_cluster_view());
        placement_->set_sched_context(nullptr);
        if (r.outcome == PlacementOutcome::PLACED) {
            commit_placement(head, r);
            continue;
        }
        if (r.outcome == PlacementOutcome::DROP) {
            remove_from_pending(head);
            head->status = JobStatus::DROPPED;
            logger->warn("job {} DROPPED: {}", head->job_id, r.reason);
            continue;
        }

        // DEFER: `head` is the pivot. Compute its count-based reservation from
        // currently-running jobs' projected completion times, then backfill any
        // reservation-safe candidate that can place now.
        // Failed NPUs are permanently unavailable, so they reduce the usable
        // capacity that EASY's count-based reservation reasons about — exactly
        // like a smaller cluster. (No-op when no NPUs are failed.)
        const int free_now = static_cast<int>(all_sys_.size()) -
                             static_cast<int>(busy_npus_.size()) -
                             static_cast<int>(failed_npus_.size());
        std::vector<RunningJob> running;
        for (const auto& kv : registry_.jobs()) {
            const JobInstance* j = kv.second.get();
            if (j->status != JobStatus::RUNNING) {
                continue;
            }
            const Tick exec = j->execution_time.value_or(now);
            const auto est = static_cast<Tick>(j->est_duration.value_or(0));
            running.push_back(
                RunningJob{std::max(now, exec + est), j->num_ranks});
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
        std::sort(candidates.begin(), candidates.end(),
                  [](const JobInstance* a, const JobInstance* b) {
                      if (a->arrival_time != b->arrival_time) {
                          return a->arrival_time < b->arrival_time;
                      }
                      return a->job_id < b->job_id;
                  });
        for (JobInstance* cand : candidates) {
            if (cand == head) {
                continue;  // never backfill the pivot itself
            }
            const Tick cand_end =
                now + static_cast<Tick>(cand->est_duration.value_or(0));
            if (!backfill_safe(res, cand_end, cand->num_ranks, extra)) {
                continue;
            }
            auto ctx = make_sched_context(cand);
            placement_->set_sched_context(&ctx);
            auto cr = placement_->try_place(*cand, snapshot_cluster_view());
            placement_->set_sched_context(nullptr);
            if (cr.outcome == PlacementOutcome::PLACED) {
                commit_placement(cand, cr);
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

bool SchedRuntime::simulation_done() const {
    return next_arrival_idx_ == static_cast<int>(arrivals_.size()) &&
           pending_.empty() && registry_.no_running_jobs();
}

void SchedRuntime::run() {
    schedule_all_arrival_sentinels();
    while (true) {
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
