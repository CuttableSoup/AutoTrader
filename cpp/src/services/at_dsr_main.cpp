// at_dsr: reads {returns: [...], trial_sharpes_per_period: [...], periods_per_year} as JSON on
// stdin, prints DsrResult::to_json() to stdout. Contract implemented in backtester/dsr_cli.cpp
// (unit-tested in cpp/tests/test_dsr_cli.cpp) so this main() is only stdin/stdout plumbing.
//
// Exists so Python research code (python/autotrader/research/sweep.py) can reuse the one tested
// Deflated Sharpe Ratio implementation (Bailey & Lopez de Prado 2014, cpp/src/backtester/
// metrics.cpp) instead of porting the formula -- every gate in this repo already goes through
// this code path via the backtester's walk-forward.
#include "backtester/dsr_cli.hpp"

#include <iostream>
#include <sstream>

int main() {
    try {
        std::ostringstream buf;
        buf << std::cin.rdbuf();
        std::cout << at::dsr_cli_run(nlohmann::json::parse(buf.str())).dump() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "at_dsr: " << e.what() << "\n";
        return 1;
    }
}
