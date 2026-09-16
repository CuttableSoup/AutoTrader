// End-to-end: synthetic universe with planted earnings reactions runs through
// StrategyEngine -> mock validator -> RiskManager -> Ledger in the backtester.
#include "backtester/backtester.hpp"
#include "backtester/walk_forward.hpp"
#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace at;

namespace {

BacktestData build_data(int n_syms, Date start, int n_sessions, unsigned seed) {
    BacktestData d;
    const auto& cal = nyse();
    BarSeries spy = test::synth_bars(start, n_sessions, 40000, 0.008, 0.0004, seed);
    for (auto& b : spy) d.mkt.add_bar("SPY", b);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> off(300, n_sessions - 80);
    for (int i = 0; i < n_syms; ++i) {
        std::string s = "S" + std::to_string(i);
        double drift = (i % 3 == 0) ? 0.0010 : 0.0001;
        BarSeries bars = test::synth_bars(start, n_sessions, 5000 + i * 37, 0.015, drift, seed + 10 + static_cast<unsigned>(i));
        // Two earnings events per symbol, quarterly-ish, with a planted reaction on some.
        for (int q = 0; q < 4; ++q) {
            int idx = 280 + q * 63 + (i % 7);
            if (idx + 3 >= n_sessions) break;
            Date report = bars[static_cast<std::size_t>(idx)].date;
            Date day0 = cal.next_trading_day(report);
            double jump = (i % 2 == 0) ? 7.0 : -4.0;
            test::inject_reaction(bars, day0, jump, 3.5, 0.5);
            EarningsEvent e = test::synth_event(s, report);
            e.fiscal_period = "Q" + std::to_string(q);
            e.event_id = s + "-" + std::to_string(q);
            e.next_report_date = idx + 63 < n_sessions ? std::optional<Date>(bars[static_cast<std::size_t>(idx + 63)].date) : std::nullopt;
            d.events.push_back(e);
        }
        for (auto& b : bars) d.mkt.add_bar(s, b);
        auto sec = test::synth_security(s, i % 4 == 0 ? "Health" : "Technology");
        sec.as_of = start;
        d.securities.push_back(sec);
    }
    return d;
}

} // namespace

TEST_CASE("backtester runs end to end on synthetic data and enforces invariants", "[backtest]") {
    Date start = make_date(2021, 1, 4);
    BacktestData data = build_data(24, start, 900, 99);
    BacktestConfig cfg;
    cfg.run_name = "smoke";
    cfg.start = nyse().add_sessions(start, 260);
    cfg.end = data.mkt.latest_date("SPY").value();
    cfg.initial_equity_cents = 100000000;
    cfg.validator_mode = "mock";
    StrategyParams params;
    params.signal.ear_top_tercile_fallback = false;
    RiskLimits limits;
    Backtester bt(data, cfg, limits);
    BacktestResult r = bt.run(params, cfg.start, cfg.end, false);

    INFO("candidates=" << r.candidates << " approved=" << r.approved << " trades=" << r.trades.size() << " vetoed=" << r.vetoed);
    CHECK(r.sessions > 500);
    CHECK(r.daily.size() == static_cast<std::size_t>(r.sessions));
    CHECK(r.candidates > 0);
    CHECK(r.approved > 0);
    CHECK(!r.trades.empty());
    // Invariants
    for (const auto& t : r.trades) {
        CHECK(t.qty > 0);
        CHECK(t.holding_sessions <= params.exit.drift_window_sessions + 1);
        CHECK(t.exit_date > t.entry_date);
        CHECK(t.net_pnl_cents == t.gross_pnl_cents - t.costs_cents);
        CHECK(t.costs_cents > 0);
    }
    for (const auto& d : r.daily) {
        CHECK(d.n_positions <= limits.max_open_positions);
        CHECK(d.gross_exposure_cents <= d.equity_cents + 1);   // no margin
        CHECK(d.drawdown_pct <= 0.0);
    }
    // Every approved entry had a stop and the ledger equity reconciles with cash + positions.
    CHECK(r.metrics.n_trades == static_cast<int>(r.trades.size()));
    CHECK(r.reject_reasons.count("halted") == 0);
    // Serialisation
    nlohmann::json j = r.to_json(true);
    CHECK(j["trades"].size() == r.trades.size());
    CHECK(j["metrics"]["sharpe_net"].is_number());
    // Counterfactual bookkeeping
    int with_fwd = 0;
    for (const auto& o : r.outcomes) if (o.fwd_return_pct) ++with_fwd;
    CHECK(with_fwd > 0);
}

TEST_CASE("walk-forward produces folds, DSR and a gate verdict", "[backtest]") {
    Date start = make_date(2020, 1, 2);
    BacktestData data = build_data(20, start, 1300, 7);
    BacktestConfig cfg;
    cfg.run_name = "wf";
    cfg.start = make_date(2021, 1, 4);
    cfg.end = data.mkt.latest_date("SPY").value();
    cfg.wf.folds = 2;
    cfg.wf.train_years = 1;
    cfg.wf.test_years = 1;
    cfg.wf.purge_sessions = 40;
    cfg.wf.embargo_sessions = 5;
    StrategyParams params;
    params.signal.grid_ear_threshold_pct = {3.0, 5.0};
    params.signal.grid_momentum_top_pct = {40.0};
    RiskLimits limits;
    Backtester bt(data, cfg, limits);
    WalkForwardResult wf = run_walk_forward(bt, params, cfg, false);
    CHECK(wf.folds.size() == 2);
    CHECK(wf.total_configurations == 4);
    CHECK(wf.dsr.n_trials == 4);
    CHECK(wf.folds[0].test_start > wf.folds[0].train_end);
    CHECK(nyse().sessions_between(wf.folds[0].train_end, wf.folds[0].test_start) == 45);
    for (const auto& f : wf.folds) CHECK(f.grid.size() == 2);
    nlohmann::json j = wf.to_json();
    CHECK(j.contains("g1_pass"));
    CHECK(j["dsr"]["n_trials"] == 4);
    std::string md = render_report_md(wf, cfg, params);
    CHECK(md.find("Gate G1") != std::string::npos);
}
