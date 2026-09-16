#include "backtester/metrics.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace at;
using Catch::Approx;

TEST_CASE("normal cdf / ppf", "[metrics]") {
    CHECK(norm_cdf(0) == Approx(0.5));
    CHECK(norm_cdf(1.96) == Approx(0.975).epsilon(1e-3));
    CHECK(norm_ppf(0.5) == Approx(0.0).margin(1e-9));
    CHECK(norm_ppf(0.975) == Approx(1.959964).epsilon(1e-5));
    CHECK(norm_ppf(0.001) == Approx(-3.090232).epsilon(1e-5));
    for (double p : {0.01, 0.1, 0.3, 0.7, 0.9, 0.99}) CHECK(norm_cdf(norm_ppf(p)) == Approx(p).epsilon(1e-9));
}

TEST_CASE("performance metrics on a deterministic series", "[metrics]") {
    std::vector<double> r(252, 0.001);
    std::vector<ClosedTrade> trades;
    ClosedTrade w; w.net_pnl_cents = 100; w.holding_sessions = 10; w.costs_cents = 5;
    ClosedTrade l; l.net_pnl_cents = -50; l.holding_sessions = 20; l.costs_cents = 5;
    trades.push_back(w); trades.push_back(l); trades.push_back(w);
    PerformanceMetrics m = compute_metrics(r, {}, trades);
    CHECK(m.n_days == 252);
    CHECK(m.total_return_pct == Approx((std::pow(1.001, 252) - 1) * 100).epsilon(1e-9));
    CHECK(m.sharpe_net == 0.0);   // zero vol
    CHECK(m.max_drawdown_pct == 0.0);
    CHECK(m.n_trades == 3);
    CHECK(m.win_rate_pct == Approx(66.666).epsilon(1e-3));
    CHECK(m.profit_factor == Approx(4.0));
    CHECK(m.avg_holding_sessions == Approx(13.333).epsilon(1e-3));
    std::vector<double> dd{0.1, -0.5, 0.2};
    CHECK(max_drawdown_from_returns(dd) == Approx(0.5));
}

TEST_CASE("deflated Sharpe penalises the number of trials", "[metrics]") {
    std::mt19937 rng(11);
    std::normal_distribution<double> z(0.0005, 0.01);
    std::vector<double> r;
    for (int i = 0; i < 750; ++i) r.push_back(z(rng));
    DsrResult one = deflated_sharpe(r, {}, 252);
    CHECK(one.n_trials == 1);
    CHECK(one.sr0_per_period == 0.0);
    CHECK(one.deflated_sharpe_annual == Approx(one.sr_annual));
    CHECK(one.psr == Approx(one.dsr_probability));
    CHECK(one.psr > 0.5);
    std::vector<double> trials;
    std::normal_distribution<double> t(0.0, 0.05);
    for (int i = 0; i < 36; ++i) trials.push_back(t(rng));
    DsrResult many = deflated_sharpe(r, trials, 252);
    CHECK(many.n_trials == 36);
    CHECK(many.sr0_per_period > 0);
    CHECK(many.deflated_sharpe_annual < one.deflated_sharpe_annual);
    CHECK(many.dsr_probability < one.dsr_probability);
    CHECK(deflated_sharpe(std::vector<double>{0.1, 0.2}, {}, 252).T == 2);
}
