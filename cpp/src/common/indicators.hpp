// Daily-bar indicators used by the strategy, the risk manager and the
// backtester. All functions take a bar series sorted ascending by date and an
// exclusive end index so the same code runs point-in-time in replay and live.
#pragma once
#include "money.hpp"
#include "time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace at {

struct Bar {
    Date date{};
    Cents open = 0;
    Cents high = 0;
    Cents low = 0;
    Cents close = 0;
    std::int64_t volume = 0;
};

using BarSeries = std::vector<Bar>;

// Index of the bar with this date, or the insertion point if absent.
std::size_t lower_bound_date(const BarSeries& s, Date d);
// Index of the bar with this date; nullopt if absent.
std::optional<std::size_t> index_of_date(const BarSeries& s, Date d);

// Simple moving average of closes over [end-n, end).
std::optional<double> sma_close(const BarSeries& s, std::size_t end, int n);

// Average True Range over the last n bars ending at end-1 (needs n+1 bars).
// Simple mean of TR, not Wilder-smoothed. Result in cents.
std::optional<Cents> atr(const BarSeries& s, std::size_t end, int n);

// Wilder RSI on closes, `period` lookback, evaluated at end-1.
std::optional<double> rsi(const BarSeries& s, std::size_t end, int period);

// 12-1 momentum: close[end-1-skip] / close[end-1-lookback] - 1 (fraction).
std::optional<double> momentum_skip(const BarSeries& s, std::size_t end, int lookback, int skip);

// Annualised realised volatility of log close-to-close returns over the last
// n returns ending at end-1 (fraction, e.g. 0.25 = 25%).
std::optional<double> realized_vol_annual(const BarSeries& s, std::size_t end, int n);

// Average daily volume (shares) over [end-n, end).
std::optional<double> adv_shares(const BarSeries& s, std::size_t end, int n);
// Average daily dollar volume in cents over [end-n, end): mean(close * volume).
std::optional<Cents> adv_dollars_cents(const BarSeries& s, std::size_t end, int n);

// Fraction return between two prices.
double simple_return(Cents from, Cents to);

// Percentile rank of x within values: percent of values <= x, in [0, 100].
double percentile_rank(std::span<const double> values, double x);

// Median of a copy of the values.
double median(std::span<const double> values);

// Sharpe ratio of a series of per-period returns, annualised with periods_per_year.
double sharpe(std::span<const double> returns, double periods_per_year, double rf_per_period = 0.0);

double mean(std::span<const double> v);
double stdev(std::span<const double> v);   // sample (n-1)
double skewness(std::span<const double> v);
double kurtosis(std::span<const double> v); // non-excess (normal = 3)

} // namespace at
