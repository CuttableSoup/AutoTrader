#include "strategy/engine.hpp"

#include "strategy/tsmom_sizing.hpp"

#include <algorithm>
#include <cstdio>

namespace at {

namespace {
std::string fmt(const char* f, double v) { char b[64]; std::snprintf(b, sizeof b, f, v); return b; }
} // namespace

StrategyEngine::StrategyEngine(StrategyParams params, const TradingCalendar& cal) : params_(std::move(params)), cal_(cal) {}

void StrategyEngine::on_earnings_event(const EarningsEvent& ev) {
    auto it = events_.find(ev.event_id);
    if (it == events_.end()) { events_[ev.event_id] = ev; return; }
    // Merge: never lose actuals or next_report_date already known.
    EarningsEvent merged = ev;
    if (!merged.eps_actual) merged.eps_actual = it->second.eps_actual;
    if (!merged.eps_consensus) merged.eps_consensus = it->second.eps_consensus;
    if (!merged.eps_consensus_asof) merged.eps_consensus_asof = it->second.eps_consensus_asof;
    if (!merged.revenue_actual_cents) merged.revenue_actual_cents = it->second.revenue_actual_cents;
    if (!merged.revenue_consensus_cents) merged.revenue_consensus_cents = it->second.revenue_consensus_cents;
    if (!merged.next_report_date) merged.next_report_date = it->second.next_report_date;
    if (merged.timing == EarningsEvent::Timing::UNKNOWN) merged.timing = it->second.timing;
    it->second = merged;
}

std::optional<Date> StrategyEngine::next_report_date(const std::string& symbol, Date after) const {
    std::optional<Date> best;
    for (const auto& [id, ev] : events_) {
        if (ev.symbol != symbol || ev.report_date <= after) continue;
        if (!best || ev.report_date < *best) best = ev.report_date;
    }
    return best;
}

std::vector<const EarningsEvent*> StrategyEngine::events_for_signal_session(Date session) const {
    std::vector<const EarningsEvent*> out;
    for (const auto& [id, ev] : events_) {
        // Cheap prefilter: report date within 5 calendar days before the session.
        int dd = days_between(ev.report_date, session);
        if (dd < 0 || dd > 6) continue;
        if (cal_.next_trading_day(ev.day0(cal_)) == session) out.push_back(&ev);
    }
    std::sort(out.begin(), out.end(), [](const EarningsEvent* a, const EarningsEvent* b) { return a->symbol < b->symbol; });
    return out;
}

StrategyEngine::SessionResult StrategyEngine::evaluate_session(Date session, const std::string& data_as_of_utc) {
    SessionResult res;
    SignalContext ctx{mkt_, universe_, cal_, session, data_as_of_utc, {}, std::nullopt};
    ctx.universe_momentum = universe_momentum_distribution(mkt_, universe_, session, params_.signal);
    res.spy_above_trend = trend_filter(mkt_, session, params_.signal);

    // Tercile cutoff from EARs of announcers whose signal session fell in the trailing 5 sessions.
    Date window_start = cal_.add_sessions(session, -5);
    while (!recent_ears_.empty() && recent_ears_.front().first < window_start) recent_ears_.pop_front();
    if (params_.signal.ear_top_tercile_fallback && recent_ears_.size() >= 6) {
        std::vector<double> ears;
        for (const auto& [d, e] : recent_ears_) ears.push_back(e);
        std::sort(ears.begin(), ears.end());
        ctx.ear_tercile_cutoff_pct = ears[ears.size() * 2 / 3];
    }

    for (const EarningsEvent* ev : events_for_signal_session(session)) {
        if (emitted_event_ids_.count(ev->event_id)) continue;
        // Fill next_report_date from later scheduled events if the event itself lacks it.
        EarningsEvent evc = *ev;
        if (!evc.next_report_date) evc.next_report_date = next_report_date(evc.symbol, evc.report_date);
        SignalEvaluation e = evaluate_earnings_signal(evc, ctx, params_);
        if (e.ear.ok) recent_ears_.emplace_back(session, e.ear.ear_pct);
        if (e.passed && e.candidate) {
            res.candidates.push_back(*e.candidate);
            emitted_event_ids_.insert(ev->event_id);
        }
        res.evaluations.push_back(std::move(e));
    }
    return res;
}

StrategyEngine::RebalanceSessionResult StrategyEngine::evaluate_rebalance_session(Date session, const std::string& data_as_of_utc) {
    RebalanceSessionResult res;
    if (!is_formation_session(session, cal_)) return res;

    TsmomSignalContext ctx{mkt_, universe_, cal_, session};
    res.signals.reserve(universe_.symbols.size());
    for (const auto& sym : universe_.symbols) res.signals.push_back(evaluate_tsmom_signal(sym, ctx, tsmom_params_));

    auto weights = compute_tsmom_target_weights(res.signals, tsmom_params_.sizing.gross_cap_pct);
    std::string month = iso_date(session).substr(0, 7);   // "YYYY-MM"

    for (const auto& w : weights) {
        std::string key = universe_.id + "|" + w.symbol + "|" + month;
        if (emitted_tsmom_keys_.count(key)) continue;
        emitted_tsmom_keys_.insert(key);

        const TsmomInstrumentSignal* sig = nullptr;
        for (const auto& s : res.signals) if (s.symbol == w.symbol) { sig = &s; break; }

        Candidate c;
        c.symbol = w.symbol;
        c.side = "BUY";
        c.signal_type = "TSMOM_ETF_V1";
        c.strategy_version = tsmom_params_.strategy_version;
        c.event_id = key;   // doubles as the idempotency key: no EarningsEvent exists for this candidate type
        c.session_date = session;
        // TSMOM candidates aren't time-critical the way an earnings reaction is, but still
        // need a real (non-default) deadline: the risk manager's pending-candidate expiry
        // sweep drops anything with entry_deadline_date < session, and a default-constructed
        // Date would make that fire immediately.
        c.entry_deadline_date = cal_.add_sessions(session, 5);
        c.universe_snapshot_id = universe_.id;
        c.data_as_of_utc = data_as_of_utc;
        c.asset_class = w.asset_class;
        c.target_weight_pct = w.target_weight * 100.0;
        if (sig && sig->ok) {
            c.mom_sign = static_cast<int>(sig->mom_sign);
            c.vol_annual_pct = sig->vol_annual * 100.0;
        }
        c.thesis_facts = {
            "12-month momentum sign " + std::to_string(c.mom_sign.value_or(0)),
            "Trailing 60-session annualized vol " + fmt("%.2f", c.vol_annual_pct.value_or(0.0)) + "%",
            "Target weight " + fmt("%+.2f", c.target_weight_pct.value_or(0.0)) + "% of equity",
        };
        res.candidates.push_back(std::move(c));
    }
    return res;
}

} // namespace at
