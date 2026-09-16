#include "backtester/walk_forward.hpp"

#include "common/indicators.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace at {

nlohmann::json FoldResult::to_json() const {
    nlohmann::json g = nlohmann::json::array();
    for (const auto& p : grid) g.push_back({{"ear_threshold_pct", p.ear_threshold_pct}, {"momentum_top_pct", p.momentum_top_pct}, {"objective", p.objective}, {"train", p.train_metrics.to_json()}});
    return {
        {"fold", fold}, {"train_start", iso_date(train_start)}, {"train_end", iso_date(train_end)}, {"test_start", iso_date(test_start)}, {"test_end", iso_date(test_end)},
        {"best", {{"ear_threshold_pct", best.ear_threshold_pct}, {"momentum_top_pct", best.momentum_top_pct}, {"objective", best.objective}}},
        {"grid", g}, {"test", test.to_json(false)},
    };
}

nlohmann::json WalkForwardResult::to_json() const {
    nlohmann::json f = nlohmann::json::array();
    for (const auto& x : folds) f.push_back(x.to_json());
    return {
        {"folds", f}, {"oos_metrics", oos_metrics.to_json()}, {"dsr", dsr.to_json()}, {"total_configurations", total_configurations},
        {"gross_sharpe_after_haircut", gross_sharpe_after_haircut}, {"folds_net_positive", folds_net_positive}, {"g1_pass", g1_pass},
        {"g1_reasons", g1_reasons}, {"counterfactual", counterfactual},
    };
}

nlohmann::json validator_counterfactual(const std::vector<CandidateOutcome>& outcomes) {
    std::vector<double> approved, vetoed;
    for (const auto& o : outcomes) {
        if (!o.fwd_return_pct) continue;
        if (o.verdict == "APPROVE") approved.push_back(*o.fwd_return_pct);
        else if (o.verdict == "REJECT") vetoed.push_back(*o.fwd_return_pct);
    }
    nlohmann::json j = {{"n_approved", approved.size()}, {"n_vetoed", vetoed.size()}, {"mean_fwd_approved_pct", nullptr}, {"mean_fwd_vetoed_pct", nullptr},
                        {"diff_pct", nullptr}, {"t_stat", nullptr}, {"verdict", "insufficient_sample"}};
    if (approved.size() < 2 || vetoed.size() < 2) return j;
    double ma = mean(approved), mv = mean(vetoed);
    double sa = stdev(approved), sv = stdev(vetoed);
    double se = std::sqrt(sa * sa / approved.size() + sv * sv / vetoed.size());
    double t = se > 0 ? (ma - mv) / se : 0.0;
    j["mean_fwd_approved_pct"] = ma;
    j["mean_fwd_vetoed_pct"] = mv;
    j["diff_pct"] = ma - mv;
    j["t_stat"] = t;
    std::size_t n = approved.size() + vetoed.size();
    if (n < 150) j["verdict"] = "insufficient_sample (<150 labelled)";
    else if (ma - mv > 0 && t > 1.5) j["verdict"] = "positive_selectivity: promote to live veto";
    else if (ma - mv < 0 && t < -1.5) j["verdict"] = "negative_selectivity: demote to tail-risk flags";
    else j["verdict"] = "no measurable lift";
    return j;
}

WalkForwardResult run_walk_forward(Backtester& bt, const StrategyParams& base, const BacktestConfig& cfg, bool verbose) {
    WalkForwardResult wf;
    const TradingCalendar& cal = nyse();
    std::vector<double> trial_sharpes;
    std::vector<CandidateOutcome> oos_outcomes;

    for (int k = 0; k < cfg.wf.folds; ++k) {
        FoldResult fr;
        fr.fold = k;
        Date train_start = make_date(year_of(cfg.start) + k * cfg.wf.test_years, unsigned(month_of(cfg.start)), unsigned(day_of(cfg.start)));
        Date train_end_nominal = make_date(year_of(train_start) + cfg.wf.train_years, unsigned(month_of(train_start)), unsigned(day_of(train_start)));
        Date test_end = make_date(year_of(train_end_nominal) + cfg.wf.test_years, unsigned(month_of(train_start)), unsigned(day_of(train_start)));
        if (test_end > cfg.end) test_end = cfg.end;
        fr.train_start = train_start;
        fr.train_end = cal.add_sessions(train_end_nominal, -cfg.wf.purge_sessions);   // purge: drop overlapping-label tail
        fr.test_start = cal.add_sessions(train_end_nominal, cfg.wf.embargo_sessions);  // embargo
        fr.test_end = test_end;
        if (fr.test_start >= fr.test_end) { spdlog::warn("walk-forward: fold {} has no test window; data too short", k); break; }
        spdlog::info("walk-forward fold {}: train {}..{} test {}..{}", k, iso_date(fr.train_start), iso_date(fr.train_end), iso_date(fr.test_start), iso_date(fr.test_end));

        bool have_best = false;
        for (double ear : base.signal.grid_ear_threshold_pct) {
            for (double mom : base.signal.grid_momentum_top_pct) {
                StrategyParams p = base;
                p.signal.ear_threshold_pct = ear;
                p.signal.momentum_top_pct = mom;
                BacktestResult r = bt.run(p, fr.train_start, fr.train_end, false);
                GridPoint g;
                g.ear_threshold_pct = ear;
                g.momentum_top_pct = mom;
                g.train_metrics = r.metrics;
                g.objective = cfg.wf.objective == "total_return" ? r.metrics.total_return_pct : r.metrics.sharpe_net;
                double sd = stdev(r.daily_net);
                g.sharpe_per_period = sd > 0 ? mean(r.daily_net) / sd : 0.0;
                trial_sharpes.push_back(g.sharpe_per_period);
                ++wf.total_configurations;
                if (verbose) spdlog::info("  grid ear={} mom={} sharpe_net={:.3f} trades={}", ear, mom, r.metrics.sharpe_net, r.metrics.n_trades);
                if (!have_best || g.objective > fr.best.objective) { fr.best = g; have_best = true; }
                fr.grid.push_back(g);
            }
        }
        StrategyParams chosen = base;
        chosen.signal.ear_threshold_pct = fr.best.ear_threshold_pct;
        chosen.signal.momentum_top_pct = fr.best.momentum_top_pct;
        fr.test = bt.run(chosen, fr.test_start, fr.test_end, false);
        spdlog::info("  fold {} chosen ear={} mom={} -> OOS sharpe_net={:.3f} return={:.2f}% trades={}", k, fr.best.ear_threshold_pct, fr.best.momentum_top_pct,
                     fr.test.metrics.sharpe_net, fr.test.metrics.total_return_pct, fr.test.metrics.n_trades);
        if (fr.test.metrics.total_return_pct > 0) ++wf.folds_net_positive;
        wf.oos_daily_net.insert(wf.oos_daily_net.end(), fr.test.daily_net.begin(), fr.test.daily_net.end());
        wf.oos_daily_gross.insert(wf.oos_daily_gross.end(), fr.test.daily_gross.begin(), fr.test.daily_gross.end());
        wf.oos_trades.insert(wf.oos_trades.end(), fr.test.trades.begin(), fr.test.trades.end());
        oos_outcomes.insert(oos_outcomes.end(), fr.test.outcomes.begin(), fr.test.outcomes.end());
        wf.folds.push_back(std::move(fr));
    }

    wf.oos_metrics = compute_metrics(wf.oos_daily_net, wf.oos_daily_gross, wf.oos_trades, 252.0, cfg.report.risk_free_annual_pct);
    wf.dsr = deflated_sharpe(wf.oos_daily_net, trial_sharpes, 252.0);
    wf.gross_sharpe_after_haircut = wf.oos_metrics.sharpe_gross * (1.0 - cfg.report.haircut_pct / 100.0);
    wf.counterfactual = validator_counterfactual(oos_outcomes);

    // G1 gate
    wf.g1_pass = true;
    if (!(wf.dsr.deflated_sharpe_annual > 0)) { wf.g1_pass = false; wf.g1_reasons.push_back("deflated Sharpe <= 0"); }
    int needed = std::max(1, (cfg.wf.folds * 2 + 2) / 3); // 2 of 3
    if (wf.folds_net_positive < needed) { wf.g1_pass = false; wf.g1_reasons.push_back("net return positive in only " + std::to_string(wf.folds_net_positive) + "/" + std::to_string(wf.folds.size()) + " folds"); }
    if (wf.gross_sharpe_after_haircut < cfg.report.min_gross_sharpe_after_haircut) { wf.g1_pass = false; wf.g1_reasons.push_back("gross Sharpe after haircut " + std::to_string(wf.gross_sharpe_after_haircut) + " < " + std::to_string(cfg.report.min_gross_sharpe_after_haircut)); }
    if (wf.oos_trades.size() < 30) { wf.g1_pass = false; wf.g1_reasons.push_back("fewer than 30 out-of-sample trades"); }
    if (wf.folds.empty()) { wf.g1_pass = false; wf.g1_reasons.push_back("no folds ran"); }
    return wf;
}

std::string render_report_md(const WalkForwardResult& wf, const BacktestConfig& cfg, const StrategyParams& base) {
    std::ostringstream o;
    o << "# Backtest report: " << cfg.run_name << "\n\n";
    o << "Strategy `" << base.strategy_version << "` (params fingerprint `" << base.fingerprint() << "`), data `" << cfg.data_dir.string() << "`, "
      << iso_date(cfg.start) << " to " << iso_date(cfg.end) << ". Validator mode: " << cfg.validator_mode << ".\n\n";
    o << "## Gate G1: " << (wf.g1_pass ? "PASS" : "FAIL") << "\n\n";
    for (const auto& r : wf.g1_reasons) o << "* " << r << "\n";
    if (wf.g1_reasons.empty()) o << "* all conditions met\n";
    o << "\n## Out-of-sample (concatenated test folds)\n\n";
    const auto& m = wf.oos_metrics;
    o << "| Metric | Value |\n|---|---|\n";
    o << "| Sessions | " << m.n_days << " |\n";
    o << "| Trades | " << m.n_trades << " |\n";
    o << "| Net return | " << m.total_return_pct << "% |\n";
    o << "| CAGR | " << m.cagr_pct << "% |\n";
    o << "| Sharpe (net) | " << m.sharpe_net << " |\n";
    o << "| Sharpe (gross) | " << m.sharpe_gross << " |\n";
    o << "| Gross Sharpe after " << cfg.report.haircut_pct << "% haircut | " << wf.gross_sharpe_after_haircut << " |\n";
    o << "| Deflated Sharpe (annual) | " << wf.dsr.deflated_sharpe_annual << " |\n";
    o << "| DSR probability | " << wf.dsr.dsr_probability << " |\n";
    o << "| PSR | " << wf.dsr.psr << " |\n";
    o << "| Configurations tried | " << wf.total_configurations << " |\n";
    o << "| Max drawdown | " << m.max_drawdown_pct << "% |\n";
    o << "| Win rate | " << m.win_rate_pct << "% |\n";
    o << "| Profit factor | " << m.profit_factor << " |\n";
    o << "| Avg holding (sessions) | " << m.avg_holding_sessions << " |\n";
    o << "| Total costs | $" << m.total_costs_dollars << " |\n";
    o << "\n## Folds\n\n| Fold | Train | Test | EAR thr | Mom top% | OOS Sharpe net | OOS return | Trades |\n|---|---|---|---|---|---|---|---|\n";
    for (const auto& f : wf.folds)
        o << "| " << f.fold << " | " << iso_date(f.train_start) << "..." << iso_date(f.train_end) << " | " << iso_date(f.test_start) << "..." << iso_date(f.test_end)
          << " | " << f.best.ear_threshold_pct << " | " << f.best.momentum_top_pct << " | " << f.test.metrics.sharpe_net << " | " << f.test.metrics.total_return_pct << "% | " << f.test.metrics.n_trades << " |\n";
    o << "\n## Validator counterfactual (mock)\n\n```\n" << wf.counterfactual.dump(2) << "\n```\n";
    o << "\nMock rules COUNTER_GAP and SECOND_8K_IN_WINDOW use post-signal data (lookahead proxies). Their lift is an upper bound on what a real validator could add, never evidence of edge.\n";
    o << "\nHaircut and DSR are reported so nobody reasons about live returns from raw backtest numbers (docs/DESIGN.md 7.1, 13).\n";
    return o.str();
}

} // namespace at
