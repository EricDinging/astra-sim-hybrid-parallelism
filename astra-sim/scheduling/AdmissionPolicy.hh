/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_ADMISSIONPOLICY_HH__
#define __ASTRASIM_SCHEDULING_ADMISSIONPOLICY_HH__

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AstraSim {
namespace Scheduling {

class JobInstance;
class ClusterView;

// Configuration the duration estimator needs (duration-keyed admission
// policies). fifo and size-only policies ignore all of it.
struct AdmissionConfig {
    std::string estimator_name = "roofline-comm";
    double peak_perf = 0.0;     // FLOP/s
    double local_mem_bw = 0.0;  // bytes/s
    double max_link_bw = 0.0;   // bytes/s
};

// Admission policy: decides WHICH pending job to attempt next. View-aware
// selector — the ordering may depend on both job state and cluster state.
class AdmissionPolicy {
  public:
    virtual ~AdmissionPolicy() = default;

    // Called once when a job arrives, before it enters the pending set. Lets a
    // policy precompute and cache an admission key (duration-keyed policies
    // read the trace here).
    virtual void on_arrival(JobInstance& job) {}

    // Next pending job to attempt placement for, or nullptr if none should be
    // admitted right now.
    virtual JobInstance* select_next(const std::vector<JobInstance*>& pending,
                                     const ClusterView& view) const = 0;

    // True if this policy drives the sweep itself (backfilling) instead of
    // being polled one job at a time via select_next. SchedRuntime::sweep
    // dispatches to easy_sweep() when this is true. Default: false.
    virtual bool uses_backfill() const {
        return false;
    }

    // True if a DEFERed job should be SKIPPED (left pending) and the sweep
    // continue down the queue, rather than strict head-of-line blocking where
    // the first DEFER stops the sweep. SchedRuntime::sweep dispatches to
    // greedy_sweep() when this is true and uses_backfill() is false — admitting
    // every job that fits in the policy's order (finite-bin First-Fit-
    // Decreasing when paired with descending-size ordering). Default: false.
    virtual bool skips_on_defer() const {
        return false;
    }

    virtual std::string name() const = 0;
};

// Shared selection scan for keyed admission policies: the job with the
// smallest (key(job), arrival_time, job_id) wins; nullptr on empty pending.
JobInstance* select_min_by_key(
    const std::vector<JobInstance*>& pending,
    const std::function<uint64_t(const JobInstance&)>& key);

// Shared selection scan for largest-first admission policies: the job with the
// largest key wins; ties broken by earliest arrival_time, then lowest job_id
// (FIFO among key-equals — the same fairness rule as select_min_by_key, NOT
// mirrored). nullptr on empty pending.
JobInstance* select_max_by_key(
    const std::vector<JobInstance*>& pending,
    const std::function<uint64_t(const JobInstance&)>& key);

// Returns nullptr on an unknown admission name or unsupported estimator
// (caller hard-exits with a clearer message).
std::unique_ptr<AdmissionPolicy> make_admission_policy(
    const std::string& name, const AdmissionConfig& cfg);

}  // namespace Scheduling
}  // namespace AstraSim

#endif
