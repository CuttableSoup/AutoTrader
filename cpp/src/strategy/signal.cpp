#include "strategy/signal.hpp"

#include <cmath>
#include <cstdio>

namespace at {

namespace {
std::string fmt(const char* f, double v) { char b[64]; std::snprintf(b, sizeof b, f, v); return b; }
} // namespace

EarComputation compute_ear(const EarningsEvent& ev, const MarketStore& mkt, const TradingCalendar& cal, const SignalParams& p) {
    EarComputation r;
    r.day0 = ev.day0(cal);
    r.day_minus1 = cal.prev_trading_day(r.day0);
    r.day_plus1 = cal.next_trading_day(r.day0);
    const BarSeries* s = mkt.bars(ev.symbol);
    if (!s) { r.error = "no bars for " + ev.symbol; return r; }
    auto c_m1 = mkt.close_on(ev.symbol, r.day_minus1);
    auto c_p1 = mkt.close_on(ev.symbol, r.day_plus1);
    auto b0 = mkt.bar_on(ev.symbol, r.day0);
    if (!c_m1 || !c_p1 || !b0) { r.error = "missing bar around day0"; return r; }
    auto b_m1 = mkt.close_on(p.ear_benchmark, r.day_minus1);
    auto b_p1 = mkt.close_on(p.ear_benchmark, r.day_plus1);
    if (!b_m1 || !b_p1) { r.error = "missing benchmark bar"; return r; }
    r.raw_return_pct = simple_return(*c_m1, *c_p1) * 100.0;
    r.benchmark_return_pct = simple_return(*b_m1, *b_p1) * 100.0;
    r.ear_pct = r.raw_return_pct - r.benchmark_return_pct;
    r.day0_volume = b0->volume;
    // ADV over the adv_window sessions strictly before day0.
    std::size_t end0 = mkt.end_index(ev.symbol, r.day_minus1);
    auto adv = adv_shares(*s, end0, p.adv_window_sessions);
    if (!adv || *adv <= 0) { r.error = "insufficient volume history"; return r; }
    r.adv20_shares = *adv;
    r.vol_ratio = static_cast<double>(r.day0_volume) / *adv;
    r.ok = true;
    return r;
}

std::optional<bool> trend_filter(const MarketStore& mkt, Date session, const SignalParams& p) {
    const BarSeries* s = mkt.bars(p.trend_symbol);
    if (!s) return std::nullopt;
    std::size_t end = mkt.end_index(p.trend_symbol, session);
    if (end == 0) return std::nullopt;
    auto ma = sma_close(*s, end, p.trend_ma_sessions);
    if (!ma) return std::nullopt;
    return static_cast<double>((*s)[end - 1].close) > *ma;
}

std::vector<double> universe_momentum_distribution(const MarketStore& mkt, const UniverseSnapshot& u, Date session, const SignalParams& p) {
    std::vector<double> out;
    out.reserve(u.symbols.size());
    for (const auto& sym : u.symbols) {
        const BarSeries* s = mkt.bars(sym);
        if (!s) continue;
        auto m = momentum_skip(*s, mkt.end_index(sym, session), p.momentum_lookback_sessions, p.momentum_skip_sessions);
        if (m) out.push_back(*m);
    }
    return out;
}

std::vector<std::string> build_thesis_facts(const EarningsEvent& ev, const EarComputation& ear, double mom_pct, std::optional<Date> next_report) {
    std::vector<std::string> f;
    f.push_back("Report date " + iso_date(ev.report_date) + " (" + EarningsEvent::timing_str(ev.timing) + "), fiscal period " + (ev.fiscal_period.empty() ? "n/a" : ev.fiscal_period));
    if (ev.eps_actual && ev.eps_consensus)
        f.push_back("EPS actual " + fmt("%.2f", *ev.eps_actual) + " vs consensus " + fmt("%.2f", *ev.eps_consensus) + " (consensus as of " + (ev.eps_consensus_asof ? iso_date(*ev.eps_consensus_asof) : "unknown") + ")");
    else if (ev.eps_actual)
        f.push_back("EPS actual " + fmt("%.2f", *ev.eps_actual) + ", consensus unavailable");
    if (ev.revenue_actual_cents && ev.revenue_consensus_cents)
        f.push_back("Revenue actual " + format_money(*ev.revenue_actual_cents) + " vs consensus " + format_money(*ev.revenue_consensus_cents));
    f.push_back("Two-day abnormal return " + fmt("%+.2f", ear.ear_pct) + "% (raw " + fmt("%+.2f", ear.raw_return_pct) + "%, benchmark " + fmt("%+.2f", ear.benchmark_return_pct) + "%)");
    f.push_back("Day-0 volume " + fmt("%.2f", ear.vol_ratio) + "x the 20-day average");
    f.push_back("12-1 month momentum percentile " + fmt("%.0f", mom_pct) + " within universe");
    if (next_report) f.push_back("Next scheduled earnings " + iso_date(*next_report));
    if (!ev.material_8k_dates.empty()) f.push_back(std::to_string(ev.material_8k_dates.size()) + " other 8-K filing(s) known as of the report date");
    return f;
}

SignalEvaluation evaluate_earnings_signal(const EarningsEvent& ev, const SignalContext& ctx, const StrategyParams& params) {
    const SignalParams& p = params.signal;
    SignalEvaluation r;
    r.ear = compute_ear(ev, ctx.mkt, ctx.cal, p);
    if (r.ear.day_plus1 != ctx.session) { r.failed.push_back("not_signal_session"); return r; }
    if (!ctx.universe.contains(ev.symbol)) { r.failed.push_back("not_in_universe"); return r; }
    if (!r.ear.ok) { r.failed.push_back("ear_unavailable:" + r.ear.error); return r; }
    if (!ev.has_actuals()) { r.failed.push_back("no_actuals"); return r; }

    // 1. EAR
    bool ear_ok = r.ear.ear_pct >= p.ear_threshold_pct;
    if (!ear_ok && p.ear_top_tercile_fallback && ctx.ear_tercile_cutoff_pct && r.ear.ear_pct > 0 && r.ear.ear_pct >= *ctx.ear_tercile_cutoff_pct)
        ear_ok = true;
    r.checked.push_back("ear_pct=" + fmt("%.2f", r.ear.ear_pct));
    if (!ear_ok) r.failed.push_back("ear");

    // 2. Volume confirmation
    r.checked.push_back("vol_ratio=" + fmt("%.2f", r.ear.vol_ratio));
    if (r.ear.vol_ratio < p.volume_multiple) r.failed.push_back("volume");

    // 3. Momentum confirmation
    const BarSeries* s = ctx.mkt.bars(ev.symbol);
    std::size_t end = ctx.mkt.end_index(ev.symbol, ctx.session);
    auto mom = momentum_skip(*s, end, p.momentum_lookback_sessions, p.momentum_skip_sessions);
    double mom_pct = 0;
    if (!mom) r.failed.push_back("momentum_history");
    else {
        mom_pct = percentile_rank(ctx.universe_momentum, *mom);
        r.checked.push_back("mom_pct=" + fmt("%.1f", mom_pct));
        if (mom_pct < 100.0 - p.momentum_top_pct) r.failed.push_back("momentum");
    }

    // 4. Trend filter
    auto trend = trend_filter(ctx.mkt, ctx.session, p);
    if (!trend) r.failed.push_back("trend_history");
    else if (!*trend) r.failed.push_back("trend");
    r.checked.push_back(std::string("spy_above_trend=") + (trend && *trend ? "1" : "0"));

    // 5. (v1.1) revision breadth — not evaluated unless enabled and data present
    if (p.revision_breadth_enabled) r.failed.push_back("revision_breadth_unavailable");

    // Optional pullback timing
    std::optional<double> rsi5;
    if (p.pullback_rsi_enabled) {
        rsi5 = rsi(*s, end, p.pullback_rsi_period);
        if (rsi5 && *rsi5 >= p.pullback_rsi_max) r.failed.push_back("rsi_pullback");
    }

    // Reference data for sizing/stops
    auto atr20 = atr(*s, end, params.exit.atr_period);
    if (!atr20 || *atr20 <= 0) r.failed.push_back("atr_history");

    if (!r.failed.empty()) return r;

    Candidate c;
    c.symbol = ev.symbol;
    c.strategy_version = params.strategy_version;
    c.event_id = ev.event_id;
    c.session_date = ctx.session;
    c.ear_pct = r.ear.ear_pct;
    c.vol_ratio = r.ear.vol_ratio;
    c.mom_pct = mom_pct;
    c.rsi5 = rsi5;
    c.entry_px_ref_cents = (*s)[end - 1].close;
    c.atr20_cents = *atr20;
    c.adv20_shares = static_cast<std::int64_t>(r.ear.adv20_shares);
    c.sector = ctx.universe.sector_of(ev.symbol);
    c.spy_above_trend = trend.value_or(false);
    c.entry_deadline_date = ctx.cal.add_sessions(ctx.session, p.entry_max_sessions_after_signal);
    c.next_report_date = ev.next_report_date;
    c.universe_snapshot_id = ctx.universe.id;
    c.thesis_facts = build_thesis_facts(ev, r.ear, mom_pct, ev.next_report_date);
    c.data_as_of_utc = ctx.data_as_of_utc;
    r.candidate = std::move(c);
    r.passed = true;
    return r;
}

} // namespace at
