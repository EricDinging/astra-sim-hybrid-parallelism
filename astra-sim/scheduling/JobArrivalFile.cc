/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/JobArrivalFile.hh"

#include "astra-sim/common/Logging.hh"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace AstraSim {
namespace Scheduling {

namespace {

std::string trim(const std::string& s) {
    const auto* ws = " \t\r\n";
    auto start = s.find_first_not_of(ws);
    if (start == std::string::npos) {
        return "";
    }
    auto end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string current;
    std::istringstream ss(line);
    while (std::getline(ss, current, ',')) {
        out.push_back(trim(current));
    }
    return out;
}

std::array<int, 3> parse_shape(const std::string& s, int row) {
    std::array<int, 3> out{};
    std::vector<int> dims;
    std::string token;
    for (char c : s) {
        if (c == 'x' || c == 'X') {
            dims.push_back(std::stoi(token));
            token.clear();
        } else {
            token += c;
        }
    }
    if (!token.empty()) {
        dims.push_back(std::stoi(token));
    }
    if (dims.size() != 3 || dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        auto logger = LoggerFactory::get_logger("scheduling");
        logger->critical("arrival CSV row {}: malformed shape '{}'", row, s);
        std::exit(1);
    }
    out[0] = dims[0];
    out[1] = dims[1];
    out[2] = dims[2];
    return out;
}

}  // namespace

std::vector<JobArrival> JobArrivalFile::parse(const std::string& path) {
    std::ifstream file(path);
    auto logger = LoggerFactory::get_logger("scheduling");
    if (!file.is_open()) {
        logger->critical("cannot open arrival file: {}", path);
        std::exit(1);
    }

    std::vector<JobArrival> out;
    std::string line;
    int row = 0;
    bool header_seen = false;
    std::set<int> seen_ids;

    while (std::getline(file, line)) {
        row++;
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        auto cols = split_csv(line);
        if (!header_seen) {
            if (cols.size() != 5 || cols[0] != "job_id" ||
                cols[1] != "arrival_time_ns" || cols[2] != "num_ranks" ||
                cols[3] != "shape" || cols[4] != "num_iterations") {
                logger->critical(
                    "arrival CSV: expected header "
                    "'job_id,arrival_time_ns,num_ranks,shape,num_iterations', "
                    "got '{}'",
                    line);
                std::exit(1);
            }
            header_seen = true;
            continue;
        }

        if (cols.size() != 5) {
            logger->critical("arrival CSV row {}: expected 5 columns, got {}",
                             row, cols.size());
            std::exit(1);
        }
        JobArrival ja;
        ja.job_id = std::stoi(cols[0]);
        ja.arrival_time = static_cast<Tick>(std::stoull(cols[1]));
        ja.num_ranks = std::stoi(cols[2]);
        ja.shape = parse_shape(cols[3], row);
        ja.num_iterations = std::stoi(cols[4]);
        if (ja.num_iterations < 1) {
            logger->critical("arrival CSV row {}: num_iterations must be >= 1",
                             row);
            std::exit(1);
        }

        if (ja.num_ranks <= 0) {
            logger->critical("arrival CSV row {}: num_ranks must be > 0", row);
            std::exit(1);
        }
        int product = ja.shape[0] * ja.shape[1] * ja.shape[2];
        if (product != ja.num_ranks) {
            logger->critical(
                "arrival CSV row {}: shape product {} != num_ranks {}", row,
                product, ja.num_ranks);
            std::exit(1);
        }
        if (!seen_ids.insert(ja.job_id).second) {
            logger->critical("arrival CSV row {}: duplicate job_id {}", row,
                             ja.job_id);
            std::exit(1);
        }

        out.push_back(ja);
    }

    if (!header_seen) {
        logger->critical("arrival CSV: missing header row");
        std::exit(1);
    }

    std::sort(out.begin(), out.end(),
              [](const JobArrival& a, const JobArrival& b) {
                  if (a.arrival_time != b.arrival_time) {
                      return a.arrival_time < b.arrival_time;
                  }
                  return a.job_id < b.job_id;
              });

    return out;
}

}  // namespace Scheduling
}  // namespace AstraSim
