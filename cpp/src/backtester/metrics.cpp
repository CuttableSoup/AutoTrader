#include "backtester/metrics.hpp"

#include "common/indicators.hpp"

#include <algorithm>
#include <cmath>

namespace at {

double norm_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

double norm_ppf(double p) {
    if (p <= 0.0) return -INFINITY;
    if (p >= 1.0) return INFINITY;
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};
    const double plow = 0.02425, phigh = 1 - plow;
    double x;
    if (p < plow) {
        double q = std::sqrt(-2 * std::log(p));
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    } else if (p <= phigh) {
        double q = p - 0.5, r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
    } else {
        double q = std::sqrt(-2 * std::log(1 - p));
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    }
    // One Newton step.
    double e = norm_cdf(x) - p;
    double u = e * std::sqrt(2 * M_PI) * std::exp(x * x / 2);
    x = x - u / (1 + x * u / 2);
    return x;
}

nlohmann::json PerformanceMetrics::to_json() const {
    return {
        {"n_days", n_days}, {"total_return_pct", total_return_pct}, {"cagr_pct", cagr_pct}, {"ann_vol_pct", ann_vol_pct},
        {"sharpe_net", sharpe_net}, {"sharpe_gross", sharpe_gross}, {"max_drawdown_pct", max_drawdown_pct}, {"n_trades", n_trades},
        {"win_rate_pct", win_rate_pct}, {"avg_net_pnl_dollars", avg_net_pnl_dollars}, {"profit_factor", profit_factor},
        {"avg_holding_sessions", avg_holding_sessions}, {"skew", skew}, {"kurtosis", kurtosis}, {"total_costs_dollars", total_costs_dollars},
    };
}

double max_drawdown_from_returns(std::span<const double> returns) {
    double eq = 1.0, peak = 1.0, mdd = 0.0;
    for (double r : returns) {
        eq *= (1.0 + r);
        peak = std::max(peak, eq);
        mdd = std::max(mdd, 1.0 - eq / peak);
    }
    return mdd;
}

PerformanceMetrics compute_metrics(std::span<const double> daily_net, std::span<const double> daily_gross, const std::vector<ClosedTrade>& trades, double periods_per_year, double rf_annual_pct) {
    PerformanceMetrics m;
    m.n_days = static_cast<int>(daily_net.size());
    double growth = 1.0;
    for (double r : daily_net) growth *= (1.0 + r);
    m.total_return_pct = (growth - 1.0) * 100.0;
    double years = m.n_days / periods_per_year;
    m.cagr_pct = years > 0 && growth > 0 ? (std::pow(growth, 1.0 / years) - 1.0) * 100.0 : 0.0;
    m.ann_vol_pct = stdev(daily_net) * std::sqrt(periods_per_year) * 100.0;
    double rf = rf_annual_pct / 100.0 / periods_per_year;
    m.sharpe_net = sharpe(daily_net, periods_per_year, rf);
    m.sharpe_gross = daily_gross.empty() ? m.sharpe_net : sharpe(daily_gross, periods_per_year, rf);
    m.max_drawdown_pct = max_drawdown_from_returns(daily_net) * 100.0;
    m.n_trades = static_cast<int>(trades.size());
    if (!trades.empty()) {
        int wins = 0; double gp = 0, gl = 0, hold = 0, net = 0, costs = 0;
        for (const auto& t : trades) {
            if (t.net_pnl_cents > 0) { ++wins; gp += static_cast<double>(t.net_pnl_cents); } else gl -= static_cast<double>(t.net_pnl_cents);
            hold += t.holding_sessions;
            net += static_cast<double>(t.net_pnl_cents);
            costs += static_cast<double>(t.costs_cents);
        }
        m.win_rate_pct = 100.0 * wins / trades.size();
        m.avg_net_pnl_dollars = net / trades.size() / 100.0;
        m.profit_factor = gl > 0 ? gp / gl : (gp > 0 ? INFINITY : 0.0);
        m.avg_holding_sessions = hold / trades.size();
        m.total_costs_dollars = costs / 100.0;
    }
    m.skew = skewness(daily_net);
    m.kurtosis = kurtosis(daily_net);
    return m;
}

nlohmann::json DsrResult::to_json() const {
    return {
        {"T", T}, {"n_trials", n_trials}, {"sr_per_period", sr_per_period}, {"sr_annual", sr_annual}, {"var_sr_trials", var_sr_trials},
        {"sr0_per_period", sr0_per_period}, {"deflated_sharpe_annual", deflated_sharpe_annual}, {"psr", psr}, {"dsr_probability", dsr_probability},
        {"skew", skew}, {"kurtosis", kurtosis},
    };
}

DsrResult deflated_sharpe(std::span<const double> returns, std::span<const double> trial_sharpes, double periods_per_year) {
    DsrResult r;
    r.T = static_cast<int>(returns.size());
    if (r.T < 3) return r;
    double sd = stdev(returns);
    r.sr_per_period = sd > 1e-12 ? mean(returns) / sd : 0.0;
    r.sr_annual = r.sr_per_period * std::sqrt(periods_per_year);
    r.skew = skewness(returns);
    r.kurtosis = kurtosis(returns);
    r.n_trials = trial_sharpes.empty() ? 1 : static_cast<int>(trial_sharpes.size());
    if (r.n_trials > 1) {
        r.var_sr_trials = stdev(trial_sharpes) * stdev(trial_sharpes);
        const double gamma = 0.5772156649015329;
        double n = static_cast<double>(r.n_trials);
        r.sr0_per_period = std::sqrt(r.var_sr_trials) * ((1 - gamma) * norm_ppf(1 - 1 / n) + gamma * norm_ppf(1 - 1 / (n * M_E)));
    }
    auto prob = [&](double sr_star) {
        double denom = std::sqrt(std::max(1e-12, 1 - r.skew * r.sr_per_period + (r.kurtosis - 1) / 4.0 * r.sr_per_period * r.sr_per_period));
        double z = (r.sr_per_period - sr_star) * std::sqrt(static_cast<double>(r.T - 1)) / denom;
        return norm_cdf(z);
    };
    r.psr = prob(0.0);
    r.dsr_probability = prob(r.sr0_per_period);
    r.deflated_sharpe_annual = (r.sr_per_period - r.sr0_per_period) * std::sqrt(periods_per_year);
    return r;
}

} // namespace at
