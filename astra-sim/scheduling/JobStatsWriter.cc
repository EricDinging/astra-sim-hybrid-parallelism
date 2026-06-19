/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/JobStatsWriter.hh"

#include "astra-sim/common/Logging.hh"
#include "astra-sim/scheduling/JobInstance.hh"
#include "astra-sim/scheduling/JobRegistry.hh"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace AstraSim {
namespace Scheduling {

namespace {

std::string shape_str(const std::array<int, 3>& s) {
    std::ostringstream oss;
    oss << s[0] << "x" << s[1] << "x" << s[2];
    return oss.str();
}

Tick avg(const std::vector<Tick>& v) {
    if (v.empty()) {
        return 0;
    }
    long double s = 0;
    for (auto t : v) {
        s += t;
    }
    return static_cast<Tick>(s / v.size());
}

Tick percentile(std::vector<Tick> v, double p) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p * (v.size() - 1));
    return v[idx];
}

}  // namespace

JobStatsWriter::JobStatsWriter(std::string output_dir)
    : output_dir_(std::move(output_dir)) {}

void JobStatsWriter::emit_jobs_csv(const JobRegistry& registry) {
    std::string path = output_dir_ + "/jobs.csv";
    std::ofstream out(path);
    if (!out.is_open()) {
        LoggerFactory::get_logger("scheduling")
            ->critical("cannot open jobs.csv for writing: {}", path);
        std::exit(1);
    }
    out << "job_id,status,num_ranks,shape,arrival_time_ns,"
           "execution_time_ns,completed_time_ns,queue_wait_ns,jct_ns\n";

    // job_id-ordered output for determinism.
    std::map<int, const JobInstance*> ordered;
    for (const auto& [id, ji] : registry.jobs()) {
        ordered[id] = ji.get();
    }
    for (const auto& [id, ji] : ordered) {
        out << ji->job_id << "," << to_string(ji->status) << ","
            << ji->num_ranks << "," << shape_str(ji->shape) << ","
            << ji->arrival_time << ",";
        if (ji->execution_time.has_value()) {
            out << *ji->execution_time;
        }
        out << ",";
        if (ji->completed_time.has_value()) {
            out << *ji->completed_time;
        }
        out << ",";
        if (ji->execution_time.has_value()) {
            out << (ji->execution_time.value() - ji->arrival_time);
        }
        out << ",";
        if (ji->completed_time.has_value()) {
            out << (ji->completed_time.value() - ji->arrival_time);
        }
        out << "\n";
    }
}

void JobStatsWriter::emit_summary(const JobRegistry& registry,
                                  const std::string& admission_name,
                                  const std::string& placement_name,
                                  int total_npus) {
    std::string path = output_dir_ + "/summary.txt";
    std::ofstream out(path);
    if (!out.is_open()) {
        LoggerFactory::get_logger("scheduling")
            ->critical("cannot open summary.txt for writing: {}", path);
        std::exit(1);
    }

    int total = 0, completed = 0, dropped = 0, defer_at_exit = 0;
    std::vector<Tick> jcts;
    std::vector<Tick> queue_waits;
    Tick min_arrival = 0;
    Tick max_completed = 0;
    bool any_arrival = false, any_completed = false;

    for (const auto& [id, ji] : registry.jobs()) {
        total++;
        if (ji->status == JobStatus::COMPLETED) {
            completed++;
            jcts.push_back(ji->completed_time.value() - ji->arrival_time);
            queue_waits.push_back(ji->execution_time.value() -
                                  ji->arrival_time);
            if (!any_arrival || ji->arrival_time < min_arrival) {
                min_arrival = ji->arrival_time;
                any_arrival = true;
            }
            if (!any_completed || ji->completed_time.value() > max_completed) {
                max_completed = ji->completed_time.value();
                any_completed = true;
            }
        } else if (ji->status == JobStatus::DROPPED) {
            dropped++;
        } else if (ji->status == JobStatus::DEFER_AT_EXIT) {
            defer_at_exit++;
        }
    }

    Tick makespan =
        (any_arrival && any_completed) ? (max_completed - min_arrival) : 0;

    out << "total_jobs:           " << total << "\n";
    out << "completed:            " << completed << "\n";
    out << "dropped:              " << dropped << "\n";
    out << "defer_at_exit:        " << defer_at_exit << "\n";
    out << "makespan_ns:          " << makespan << "\n";
    out << "mean_jct_ns:          " << avg(jcts) << "\n";
    out << "p50_jct_ns:           " << percentile(jcts, 0.50) << "\n";
    out << "p95_jct_ns:           " << percentile(jcts, 0.95) << "\n";
    out << "mean_queue_wait_ns:   " << avg(queue_waits) << "\n";
    out << "total_npus:           " << total_npus << "\n";
    out << "admission_policy:     " << admission_name << "\n";
    out << "placement_policy:     " << placement_name << "\n";
}

void JobStatsWriter::emit_node_jobs(const JobRegistry& registry,
                                    int total_npus) {
    std::string path = output_dir_ + "/node_jobs.csv";
    std::ofstream out(path);
    if (!out.is_open()) {
        LoggerFactory::get_logger("scheduling")
            ->critical("cannot open node_jobs.csv for writing: {}", path);
        std::exit(1);
    }

    std::vector<std::vector<int>> per_node(total_npus);
    for (const auto& [id, ji] : registry.jobs()) {
        for (int npu : ji->rank_map) {
            if (npu >= 0 && npu < total_npus) {
                per_node[npu].push_back(ji->job_id);
            }
        }
    }
    for (auto& v : per_node) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }

    out << "node_id,jobs_served\n";
    for (int n = 0; n < total_npus; ++n) {
        out << n;
        for (int jid : per_node[n]) {
            out << "," << jid;
        }
        out << "\n";
    }
}

void JobStatsWriter::emit_failed_nodes(const std::vector<int>& failed_npus) {
    std::string path = output_dir_ + "/failed_nodes.log";
    std::ofstream out(path);
    if (!out.is_open()) {
        LoggerFactory::get_logger("scheduling")
            ->critical("cannot open failed_nodes.log for writing: {}", path);
        std::exit(1);
    }
    out << "failed_npu_id\n";
    for (int npu : failed_npus) {
        out << npu << "\n";
    }
}

}  // namespace Scheduling
}  // namespace AstraSim
