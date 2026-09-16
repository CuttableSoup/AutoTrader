// JSON request/response contract for at_dsr (cpp/src/services/at_dsr_main.cpp), factored out of
// main() so it is unit-testable without spawning the executable (cpp/tests/test_dsr_cli.cpp).
#pragma once
#include <nlohmann/json.hpp>

namespace at {

// input: {"returns": [...], "trial_sharpes_per_period": [...] (optional), "periods_per_year": number (optional, default 252)}
// output: DsrResult::to_json(). Throws nlohmann::json::exception / std::exception on a malformed request.
nlohmann::json dsr_cli_run(const nlohmann::json& input);

} // namespace at
