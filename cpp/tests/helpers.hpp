// Shared test fixtures: deterministic synthetic bars, universes and events.
#pragma once
#include "common/calendar.hpp"
#include "strategy/market_store.hpp"
#include "strategy/types.hpp"

#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace at::test {

// Geometric random walk with optional daily drift, starting at start_px on the first session >= from.
inline BarSeries synth_bars(Date from, int n_sessions, Cents start_px, double daily_vol = 0.015, double drift = 0.0002, unsigned seed = 42, std::int64_t base_volume = 1000000) {
    const TradingCalendar& cal = nyse();
    std::mt19937 rng(seed);
    std::normal_distribution<double> z(0.0, 1.0);
    BarSeries s;
    Date d = cal.add_sessions(from, 0);
    double px = static_cast<double>(start_px);
    for (int i = 0; i < n_sessions; ++i) {
        double r = drift + daily_vol * z(rng);
        double open = px * (1 + 0.3 * daily_vol * z(rng));
        double close = px * std::exp(r);
        double high = std::max(open, close) * (1 + std::fabs(0.5 * daily_vol * z(rng)));
        double low = std::min(open, close) * (1 - std::fabs(0.5 * daily_vol * z(rng)));
        Bar b;
        b.date = d;
        b.open = static_cast<Cents>(std::llround(open));
        b.high = static_cast<Cents>(std::llround(high));
        b.low = static_cast<Cents>(std::llround(low));
        b.close = static_cast<Cents>(std::llround(close));
        b.volume = base_volume + static_cast<std::int64_t>(base_volume * 0.2 * std::fabs(z(rng)));
        s.push_back(b);
        px = close;
        d = cal.next_trading_day(d);
    }
    return s;
}

inline SecurityInfo synth_security(const std::string& sym, const std::string& sector = "Technology", Cents cap = 2000000000000LL) {
    SecurityInfo s;
    s.symbol = sym;
    s.name = sym + " Inc";
    s.sector = sector;
    s.category = "Domestic Common Stock";
    s.market_cap_cents = cap;
    s.first_listed = make_date(2010, 1, 4);
    s.analyst_coverage = 12;
    s.transcript_available = true;
    s.median_spread_bps = 2.0;
    s.as_of = make_date(2019, 12, 31);
    return s;
}

inline UniverseSnapshot synth_universe(const std::vector<std::string>& syms, Date as_of) {
    UniverseSnapshot u;
    u.id = "test-universe";
    u.as_of = as_of;
    for (const auto& s : syms) { u.symbols.push_back(s); u.info[s] = synth_security(s); }
    return u;
}

// Inject an earnings reaction: day0 close jumps by jump_pct with volume x vol_mult, day+1 drifts a bit more.
inline void inject_reaction(BarSeries& s, Date day0, double jump_pct, double vol_mult, double day1_pct = 1.0) {
    auto i = index_of_date(s, day0);
    if (!i) return;
    double f = 1.0 + jump_pct / 100.0;
    for (std::size_t k = *i; k < s.size(); ++k) {
        s[k].open = static_cast<Cents>(std::llround(s[k].open * f));
        s[k].high = static_cast<Cents>(std::llround(s[k].high * f));
        s[k].low = static_cast<Cents>(std::llround(s[k].low * f));
        s[k].close = static_cast<Cents>(std::llround(s[k].close * f));
    }
    s[*i].open = s[*i - 1].close; // gap happens at open of day0 in reality; keep open at prior close for clarity of the test
    s[*i].volume = static_cast<std::int64_t>(s[*i].volume * vol_mult);
    if (*i + 1 < s.size()) {
        double g = 1.0 + day1_pct / 100.0;
        for (std::size_t k = *i + 1; k < s.size(); ++k) {
            s[k].open = static_cast<Cents>(std::llround(s[k].open * g));
            s[k].high = static_cast<Cents>(std::llround(s[k].high * g));
            s[k].low = static_cast<Cents>(std::llround(s[k].low * g));
            s[k].close = static_cast<Cents>(std::llround(s[k].close * g));
        }
    }
}

inline EarningsEvent synth_event(const std::string& sym, Date report_date, EarningsEvent::Timing timing = EarningsEvent::Timing::AMC) {
    EarningsEvent e;
    e.symbol = sym;
    e.report_date = report_date;
    e.timing = timing;
    e.fiscal_period = "2024Q2";
    e.event_id = sym + "-" + iso_date(report_date);
    e.eps_actual = 1.25;
    e.eps_consensus = 1.10;
    e.eps_consensus_asof = add_days(report_date, -1);
    e.revenue_actual_cents = 500000000000LL;
    e.revenue_consensus_cents = 480000000000LL;
    e.next_report_date = add_days(report_date, 91);
    e.source = "synthetic";
    return e;
}

} // namespace at::test
