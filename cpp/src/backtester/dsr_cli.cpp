#include "backtester/dsr_cli.hpp"
#include "backtester/metrics.hpp"

#include <vector>

namespace at {

nlohmann::json dsr_cli_run(const nlohmann::json& input) {
    std::vector<double> returns = input.at("returns").get<std::vector<double>>();
    std::vector<double> trials = input.value("trial_sharpes_per_period", std::vector<double>{});
    double periods_per_year = input.value("periods_per_year", 252.0);
    return deflated_sharpe(returns, trials, periods_per_year).to_json();
}

} // namespace at
