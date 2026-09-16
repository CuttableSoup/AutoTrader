// Walk-forward with purge/embargo. Tunes only the two designated parameters
// (EAR threshold, momentum percentile) on each train window, evaluates the
// chosen pair out-of-sample, then reports DSR penalised for every
// configuration tried and the G1 gate verdict.
#pragma once
#include "backtester/backtester.hpp"

namespace at {

struct GridPoint {
    double ear_threshold_pct = 0;
    double momentum_top_pct = 0;
    PerformanceMetrics train_metrics;
    double objective = 0;
    double sharpe_per_period = 0;
};

struct FoldResult {
    int fold = 0;
    Date train_start{}, train_end{}, test_start{}, test_end{};
    std::vector<GridPoint> grid;
    GridPoint best;
    BacktestResult test;
    nlohmann::json to_json() const;
};

struct WalkForwardResult {
    std::vector<FoldResult> folds;
    std::vector<double> oos_daily_net;
    std::vector<double> oos_daily_gross;
    std::vector<ClosedTrade> oos_trades;
    PerformanceMetrics oos_metrics;
    DsrResult dsr;
    int total_configurations = 0;
    double gross_sharpe_after_haircut = 0;
    int folds_net_positive = 0;
    bool g1_pass = false;
    std::vector<std::string> g1_reasons;
    // Counterfactual over all OOS candidates: approved-minus-vetoed forward return, t-stat.
    nlohmann::json counterfactual;
    nlohmann::json to_json() const;
};

WalkForwardResult run_walk_forward(Backtester& bt, const StrategyParams& base, const BacktestConfig& cfg, bool verbose = false);

// Paired counterfactual for the validator layer (docs/DESIGN.md 7.3).
nlohmann::json validator_counterfactual(const std::vector<CandidateOutcome>& outcomes);

// Human-readable report.
std::string render_report_md(const WalkForwardResult& wf, const BacktestConfig& cfg, const StrategyParams& base);

} // namespace at
