// Performance metrics, Probabilistic and Deflated Sharpe Ratio
// (Bailey & Lopez de Prado 2014), max drawdown.
#pragma once
#include "strategy/ledger.hpp"

#include <nlohmann/json.hpp>

#include <span>
#include <vector>

namespace at {

double norm_cdf(double x);
double norm_ppf(double p);   // inverse CDF, Acklam's algorithm with one Newton refinement

struct PerformanceMetrics {
    int n_days = 0;
    double total_return_pct = 0;
    double cagr_pct = 0;
    double ann_vol_pct = 0;
    double sharpe_net = 0;
    double sharpe_gross = 0;
    double max_drawdown_pct = 0;
    int n_trades = 0;
    double win_rate_pct = 0;
    double avg_net_pnl_dollars = 0;
    double profit_factor = 0;
    double avg_holding_sessions = 0;
    double skew = 0;
    double kurtosis = 3;
    double total_costs_dollars = 0;
    nlohmann::json to_json() const;
};

PerformanceMetrics compute_metrics(std::span<const double> daily_net, std::span<const double> daily_gross, const std::vector<ClosedTrade>& trades, double periods_per_year = 252.0, double rf_annual_pct = 0.0);

double max_drawdown_from_returns(std::span<const double> returns); // fraction, >= 0

struct DsrResult {
    int T = 0;                       // periods
    int n_trials = 1;
    double sr_per_period = 0;        // observed
    double sr_annual = 0;
    double var_sr_trials = 0;        // variance of trial Sharpes (per period)
    double sr0_per_period = 0;       // expected max Sharpe under the null given n_trials
    double deflated_sharpe_annual = 0; // (SR - SR0) annualised; gate: > 0
    double psr = 0;                  // P(SR > 0)
    double dsr_probability = 0;      // P(SR > SR0)
    double skew = 0;
    double kurtosis = 3;
    nlohmann::json to_json() const;
};

// trial_sharpes_per_period: per-period Sharpe of every configuration tried (empty -> n_trials = 1, SR0 = 0).
DsrResult deflated_sharpe(std::span<const double> returns, std::span<const double> trial_sharpes_per_period, double periods_per_year = 252.0);

} // namespace at
