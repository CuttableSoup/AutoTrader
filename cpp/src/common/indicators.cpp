#include "indicators.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace at {

std::size_t lower_bound_date(const BarSeries& s, Date d) {
    auto it = std::lower_bound(s.begin(), s.end(), d, [](const Bar& b, Date x) { return b.date < x; });
    return static_cast<std::size_t>(it - s.begin());
}

std::optional<std::size_t> index_of_date(const BarSeries& s, Date d) {
    auto i = lower_bound_date(s, d);
    if (i < s.size() && s[i].date == d) return i;
    return std::nullopt;
}

std::optional<double> sma_close(const BarSeries& s, std::size_t end, int n) {
    if (n <= 0 || end > s.size() || end < static_cast<std::size_t>(n)) return std::nullopt;
    double sum = 0;
    for (std::size_t i = end - static_cast<std::size_t>(n); i < end; ++i) sum += static_cast<double>(s[i].close);
    return sum / n;
}

std::optional<Cents> atr(const BarSeries& s, std::size_t end, int n) {
    if (n <= 0 || end > s.size() || end < static_cast<std::size_t>(n) + 1) return std::nullopt;
    double sum = 0;
    for (std::size_t i = end - static_cast<std::size_t>(n); i < end; ++i) {
        const Bar& b = s[i];
        Cents prev_close = s[i - 1].close;
        Cents tr = std::max<Cents>({b.high - b.low, static_cast<Cents>(std::llabs(b.high - prev_close)), static_cast<Cents>(std::llabs(b.low - prev_close))});
        sum += static_cast<double>(tr);
    }
    return static_cast<Cents>(std::llround(sum / n));
}

std::optional<double> rsi(const BarSeries& s, std::size_t end, int period) {
    if (period <= 0 || end > s.size() || end < static_cast<std::size_t>(period) + 1) return std::nullopt;
    // Wilder: seed with simple averages over the first `period` changes, then smooth over the rest.
    std::size_t start = end - static_cast<std::size_t>(period) - 1;
    // Use the full available history up to 250 bars for the smoothing warm-up when present.
    std::size_t warm_start = end > 250 ? end - 250 : 1;
    if (warm_start > start) warm_start = start + 1;
    double gain = 0, loss = 0;
    std::size_t first = warm_start;
    int cnt = 0;
    for (std::size_t i = first; i < first + static_cast<std::size_t>(period) && i < end; ++i) {
        double ch = static_cast<double>(s[i].close - s[i - 1].close);
        if (ch > 0) gain += ch; else loss -= ch;
        ++cnt;
    }
    if (cnt < period) return std::nullopt;
    double ag = gain / period, al = loss / period;
    for (std::size_t i = first + static_cast<std::size_t>(period); i < end; ++i) {
        double ch = static_cast<double>(s[i].close - s[i - 1].close);
        ag = (ag * (period - 1) + (ch > 0 ? ch : 0)) / period;
        al = (al * (period - 1) + (ch < 0 ? -ch : 0)) / period;
    }
    if (al == 0) return 100.0;
    double rs = ag / al;
    return 100.0 - 100.0 / (1.0 + rs);
}

std::optional<double> momentum_skip(const BarSeries& s, std::size_t end, int lookback, int skip) {
    if (end > s.size() || lookback <= skip || end < static_cast<std::size_t>(lookback) + 1) return std::nullopt;
    Cents recent = s[end - 1 - static_cast<std::size_t>(skip)].close;
    Cents old = s[end - 1 - static_cast<std::size_t>(lookback)].close;
    if (old <= 0) return std::nullopt;
    return simple_return(old, recent);
}

std::optional<double> realized_vol_annual(const BarSeries& s, std::size_t end, int n) {
    if (n < 2 || end > s.size() || end < static_cast<std::size_t>(n) + 1) return std::nullopt;
    std::vector<double> r;
    r.reserve(static_cast<std::size_t>(n));
    for (std::size_t i = end - static_cast<std::size_t>(n); i < end; ++i) {
        if (s[i - 1].close <= 0 || s[i].close <= 0) return std::nullopt;
        r.push_back(std::log(static_cast<double>(s[i].close) / static_cast<double>(s[i - 1].close)));
    }
    return stdev(r) * std::sqrt(252.0);
}

std::optional<double> adv_shares(const BarSeries& s, std::size_t end, int n) {
    if (n <= 0 || end > s.size() || end < static_cast<std::size_t>(n)) return std::nullopt;
    double sum = 0;
    for (std::size_t i = end - static_cast<std::size_t>(n); i < end; ++i) sum += static_cast<double>(s[i].volume);
    return sum / n;
}

std::optional<Cents> adv_dollars_cents(const BarSeries& s, std::size_t end, int n) {
    if (n <= 0 || end > s.size() || end < static_cast<std::size_t>(n)) return std::nullopt;
    long double sum = 0;
    for (std::size_t i = end - static_cast<std::size_t>(n); i < end; ++i)
        sum += static_cast<long double>(s[i].close) * static_cast<long double>(s[i].volume);
    return static_cast<Cents>(std::llround(static_cast<double>(sum / n)));
}

double simple_return(Cents from, Cents to) {
    return from == 0 ? 0.0 : static_cast<double>(to - from) / static_cast<double>(from);
}

double percentile_rank(std::span<const double> values, double x) {
    if (values.empty()) return 0.0;
    std::size_t le = 0;
    for (double v : values) if (v <= x) ++le;
    return 100.0 * static_cast<double>(le) / static_cast<double>(values.size());
}

double median(std::span<const double> values) {
    if (values.empty()) return 0.0;
    std::vector<double> v(values.begin(), values.end());
    std::sort(v.begin(), v.end());
    std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double mean(std::span<const double> v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

double stdev(std::span<const double> v) {
    if (v.size() < 2) return 0.0;
    double m = mean(v), acc = 0;
    for (double x : v) acc += (x - m) * (x - m);
    return std::sqrt(acc / static_cast<double>(v.size() - 1));
}

double skewness(std::span<const double> v) {
    if (v.size() < 3) return 0.0;
    double m = mean(v), s2 = 0, s3 = 0;
    for (double x : v) { double d = x - m; s2 += d * d; s3 += d * d * d; }
    double n = static_cast<double>(v.size());
    double sd = std::sqrt(s2 / n);
    return sd == 0 ? 0.0 : (s3 / n) / (sd * sd * sd);
}

double kurtosis(std::span<const double> v) {
    if (v.size() < 4) return 3.0;
    double m = mean(v), s2 = 0, s4 = 0;
    for (double x : v) { double d = x - m; s2 += d * d; s4 += d * d * d * d; }
    double n = static_cast<double>(v.size());
    double var = s2 / n;
    return var == 0 ? 3.0 : (s4 / n) / (var * var);
}

double sharpe(std::span<const double> returns, double periods_per_year, double rf_per_period) {
    if (returns.size() < 2) return 0.0;
    std::vector<double> ex(returns.begin(), returns.end());
    for (double& r : ex) r -= rf_per_period;
    double sd = stdev(ex);
    return sd < 1e-12 ? 0.0 : mean(ex) / sd * std::sqrt(periods_per_year);
}

} // namespace at
