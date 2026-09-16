#include "strategy/exits.hpp"

#include <algorithm>

namespace at {

const char* exit_intent_str(ExitIntent i) {
    switch (i) {
        case ExitIntent::ExitTime: return "EXIT_TIME";
        case ExitIntent::ExitEarnings: return "EXIT_EARNINGS";
        case ExitIntent::ExitTrendScale: return "EXIT_TREND_SCALE";
        case ExitIntent::ExitDrawdown: return "EXIT_DRAWDOWN";
        case ExitIntent::Flatten: return "FLATTEN";
        case ExitIntent::StopReplace: return "STOP_REPLACE";
        default: return "NONE";
    }
}

Date compute_exit_deadline(Date entry_session, std::optional<Date> next_report_date, const ExitParams& p, const TradingCalendar& cal) {
    Date time_limit = cal.add_sessions(entry_session, p.drift_window_sessions);
    if (!next_report_date) return time_limit;
    // Exit the day before the next report: last session strictly before next_report_date, then back (n-1) more.
    Date before = cal.is_trading_day(*next_report_date) ? cal.prev_trading_day(*next_report_date)
                                                         : cal.add_sessions(*next_report_date, -1);
    Date gate = cal.add_sessions(before, -(p.exit_sessions_before_earnings - 1));
    return std::min(time_limit, gate);
}

Cents initial_stop_px(Cents entry_px_cents, Cents atr_cents, const ExitParams& p) {
    Cents stop = entry_px_cents - mul(atr_cents, p.trailing_stop_atr_mult);
    return std::max<Cents>(stop, 1);
}

Cents ratchet_stop_px(std::optional<Cents> current, Cents hwm_close_cents, Cents atr_cents, const ExitParams& p) {
    Cents candidate = std::max<Cents>(hwm_close_cents - mul(atr_cents, p.trailing_stop_atr_mult), 1);
    if (!current) return candidate;
    return std::max(*current, candidate);
}

std::vector<ExitAction> evaluate_exits(const PositionState& pos, Date session, std::optional<bool> spy_above_trend, const ExitParams& p, const TradingCalendar& cal) {
    std::vector<ExitAction> out;
    if (pos.qty <= 0) return out;

    // 1. Earnings gate / time limit: deadline reached at this close -> exit next open.
    if (session >= pos.exit_deadline) {
        ExitAction a;
        bool earnings_bound = pos.next_report_date && pos.exit_deadline < cal.add_sessions(pos.entry_session, p.drift_window_sessions);
        a.intent = earnings_bound ? ExitIntent::ExitEarnings : ExitIntent::ExitTime;
        a.qty = pos.qty;
        a.reason = earnings_bound ? "next earnings " + iso_date(*pos.next_report_date) : std::to_string(p.drift_window_sessions) + " sessions elapsed";
        out.push_back(a);
        return out; // full exit dominates everything else
    }

    // 2. Trend break: scale down once by trend_break_scale_down_pct.
    if (spy_above_trend && !*spy_above_trend && !pos.scaled_down) {
        ExitAction a;
        a.intent = ExitIntent::ExitTrendScale;
        a.qty = std::max<std::int64_t>(1, static_cast<std::int64_t>(static_cast<double>(pos.qty) * p.trend_break_scale_down_pct / 100.0));
        if (a.qty >= pos.qty) a.qty = pos.qty;
        a.reason = "SPY below trend MA; scale down " + std::to_string(static_cast<int>(p.trend_break_scale_down_pct)) + "%";
        out.push_back(a);
    }

    // 3. Trailing stop ratchet (stop rests at broker; only ever moves up).
    if (pos.atr20_cents && pos.hwm_px_cents) {
        Cents new_stop = ratchet_stop_px(pos.stop_px_cents, *pos.hwm_px_cents, *pos.atr20_cents, p);
        if (!pos.stop_px_cents || new_stop > *pos.stop_px_cents) {
            ExitAction a;
            a.intent = ExitIntent::StopReplace;
            a.qty = pos.qty;
            a.new_stop_cents = new_stop;
            a.reason = "trailing stop ratchet to " + cents_to_decimal(new_stop);
            out.push_back(a);
        }
    }
    return out;
}

} // namespace at
