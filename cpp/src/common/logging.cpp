#include "logging.hpp"

#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace at {

void init_logging(const std::string& service, const std::string& log_dir, const std::string& level) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    if (!log_dir.empty()) {
        std::filesystem::create_directories(log_dir);
        sinks.push_back(std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_dir + "/" + service + ".log", 0, 0));
    }
    auto logger = std::make_shared<spdlog::logger>(service, sinks.begin(), sinks.end());
    logger->set_pattern("%Y-%m-%dT%H:%M:%S.%eZ [%n] %^%l%$ %v");
    logger->set_level(spdlog::level::from_str(level));
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
}

} // namespace at
