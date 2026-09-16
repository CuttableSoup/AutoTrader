// Unit tests for the at_dsr CLI's JSON contract (backtester/dsr_cli.cpp), against the same
// known-good cases test_metrics.cpp asserts directly on deflated_sharpe() -- this only checks
// that the JSON request/response wrapping is faithful, not the DSR math itself.
#include "backtester/dsr_cli.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace at;
using Catch::Approx;

TEST_CASE("dsr_cli_run matches deflated_sharpe with no trials", "[dsr_cli]") {
    std::mt19937 rng(11);
    std::normal_distribution<double> z(0.0005, 0.01);
    nlohmann::json returns = nlohmann::json::array();
    for (int i = 0; i < 750; ++i) returns.push_back(z(rng));
    nlohmann::json out = dsr_cli_run({{"returns", returns}, {"periods_per_year", 252}});
    CHECK(out["n_trials"] == 1);
    CHECK(out["sr0_per_period"] == Approx(0.0));
    CHECK(out["deflated_sharpe_annual"] == Approx(out["sr_annual"].get<double>()));
    CHECK(out["psr"].get<double>() > 0.5);
}

TEST_CASE("dsr_cli_run penalises trials the same way deflated_sharpe does", "[dsr_cli]") {
    std::mt19937 rng(11);
    std::normal_distribution<double> z(0.0005, 0.01);
    nlohmann::json returns = nlohmann::json::array();
    for (int i = 0; i < 750; ++i) returns.push_back(z(rng));
    nlohmann::json one = dsr_cli_run({{"returns", returns}});

    std::normal_distribution<double> t(0.0, 0.05);
    nlohmann::json trials = nlohmann::json::array();
    for (int i = 0; i < 36; ++i) trials.push_back(t(rng));
    nlohmann::json many = dsr_cli_run({{"returns", returns}, {"trial_sharpes_per_period", trials}, {"periods_per_year", 252}});

    CHECK(many["n_trials"] == 36);
    CHECK(many["sr0_per_period"].get<double>() > 0.0);
    CHECK(many["deflated_sharpe_annual"].get<double>() < one["deflated_sharpe_annual"].get<double>());
    CHECK(many["dsr_probability"].get<double>() < one["dsr_probability"].get<double>());
}

TEST_CASE("dsr_cli_run defaults periods_per_year and rejects a missing returns field", "[dsr_cli]") {
    nlohmann::json out = dsr_cli_run({{"returns", nlohmann::json::array({0.1, 0.2, 0.1, -0.05, 0.03})}});
    CHECK(out["T"] == 5);
    CHECK_THROWS(dsr_cli_run({{"trial_sharpes_per_period", nlohmann::json::array({0.1})}}));
}
