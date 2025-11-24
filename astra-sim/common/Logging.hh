#ifndef __COMMON_LOGGING_HH__
#define __COMMON_LOGGING_HH__

#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#include "spdlog_setup/conf.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace AstraSim {

// Wrapper sink that only logs messages of a specific exact level
class ExactLevelSink : public spdlog::sinks::sink {
  public:
    ExactLevelSink(std::shared_ptr<spdlog::sinks::sink> sink,
                   spdlog::level::level_enum level);

    void log(const spdlog::details::log_msg& msg) override;

    void flush() override;

    void set_pattern(const std::string& pattern) override;

    void set_formatter(
        std::unique_ptr<spdlog::formatter> sink_formatter) override;

  private:
    std::shared_ptr<spdlog::sinks::sink> sink;
    spdlog::level::level_enum level;
};

class LoggerFactory {
  public:
    LoggerFactory() = delete;
    static std::shared_ptr<spdlog::logger> get_logger(
        const std::string& logger_name);
    static void init(const std::string& log_conf_path = "empty",
                     const std::string& log_path = "log");
    static void shutdown(void);

  private:
    static void init_default_components(const std::string& log_path);
    static std::unordered_set<spdlog::sink_ptr> default_sinks;
};

}  // namespace AstraSim

#endif
