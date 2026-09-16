// Exit rules (docs/DESIGN.md 2.2): time limit, earnings gate, trailing ATR stop
// resting at the broker (ratcheted here), trend-break scale-down.
#pragma once
#include "common/calendar.hpp"
#include "strategy/params.hpp"
#include "strategy/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace at {

enum class ExitIntent { None, ExitTime, ExitEarnings, ExitTrendScale, ExitDrawdown, Flatten, StopReplace };
const char* exit_intent_str(ExitIntent i);

struct ExitAction {
    ExitIntent intent = ExitIntent::None;
    std::int64_t qty = 0;                  // shares to sell (or full position)
    std::optional<Cents> new_stop_cents;   // for StopReplace
    std::string reason;
};

// Deadline = min(entry + drift_window sessions, next_report_date - exit_sessions_before_earnings).
Date compute_exit_deadline(Date entry_session, std::optional<Date> next_report_date, const ExitParams& p, const TradingCalendar& cal);

// Initial stop for a long entered at entry_px: entry - mult * ATR (never below 1 cent).
Cents initial_stop_px(Cents entry_px_cents, Cents atr_cents, const ExitParams& p);
// Trailing ratchet: max(current, hwm - mult * ATR). Never moves down.
Cents ratchet_stop_px(std::optional<Cents> current_stop_cents, Cents hwm_close_cents, Cents atr_cents, const ExitParams& p);

// End-of-session sweep for one position. `session` is the session that just closed.
// spy_above_trend: nullopt if unknown (treated as above; no scale-down on missing data).
std::vector<ExitAction> evaluate_exits(const PositionState& pos, Date session, std::optional<bool> spy_above_trend, const ExitParams& p, const TradingCalendar& cal);

} // namespace at
