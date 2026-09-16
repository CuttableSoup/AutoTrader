// spdlog setup shared by every service: stderr + daily rotating file.
#pragma once
#include <spdlog/spdlog.h>

#include <string>

namespace at {

// level: trace|debug|info|warn|error. log_dir may be empty (stderr only).
void init_logging(const std::string& service, const std::string& log_dir, const std::string& level = "info");

} // namespace at
