// Position ledger + equity history. Produces the portfolio.state payload.
// Shared by the live portfolio service (synced from the broker) and the
// backtester (fed by simulated fills) so risk sees identical inputs.
#pragma once
#include "common/calendar.hpp"
#include "strategy/market_store.hpp"
#include "strategy/types.hpp"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace at {

struct EntryMeta {
    std::string sector;
    std::string asset_class;           // TSMOM only: also marks the position as TSMOM-owned
    Cents atr20_cents = 0;
    Cents stop_px_cents = 0;
    Date exit_deadline{};
    std::optional<Date> next_report_date;
    std::optional<std::string> candidate_msg_id;
    std::string event_id;
    double ear_pct = 0;
    double mom_pct = 0;
    std::string verdict;               // validator verdict recorded for the counterfactual
    std::optional<std::string> stop_broker_order_id;
};

struct ClosedTrade {
    std::string symbol;
    std::optional<std::string> candidate_msg_id;
    std::string event_id;
    Date entry_date{};
    Cents entry_px_cents = 0;
    std::int64_t qty = 0;
    Date exit_date{};
    Cents exit_px_cents = 0;
    std::string exit_reason;
    Cents gross_pnl_cents = 0;
    Cents costs_cents = 0;
    Cents net_pnl_cents = 0;
    int holding_sessions = 0;
    double ear_pct = 0;
    double mom_pct = 0;
    std::string verdict;
    nlohmann::json to_json() const;
    static std::string csv_header();
    std::string to_csv() const;
};

struct DailyRecord {
    Date date{};
    Cents equity_cents = 0;
    Cents cash_cents = 0;
    Cents gross_exposure_cents = 0;
    int n_positions = 0;
    double ret = 0;      // fraction
    double drawdown_pct = 0;
};

class Ledger {
public:
    explicit Ledger(Cents initial_cash_cents);

    // ---- fills ----
    void apply_entry_fill(const std::string& symbol, std::int64_t qty, Cents px, Cents costs, Date session, const std::string& ts_utc, const EntryMeta& meta);
    // qty may be partial. Records a ClosedTrade for the shares sold.
    void apply_exit_fill(const std::string& symbol, std::int64_t qty, Cents px, Cents costs, Date session, const std::string& reason);
    // TSMOM: resizes (opens/closes/flips/partially adjusts) a position to a signed
    // target_qty in one fill, unlike apply_entry_fill/apply_exit_fill which assume
    // long-only, monotonically-increasing-then-decreasing positions. Cash moves by
    // (target_qty - current_qty) * px + costs regardless of direction. Realizes P&L
    // (records a ClosedTrade) on any portion that reduces |qty| or crosses zero; a
    // crossing is implemented as a full close of the old side immediately followed by a
    // fresh open of the new side at px, composing the same math apply_exit_fill/
    // apply_entry_fill already use rather than deriving new P&L formulas.
    void apply_rebalance_fill(const std::string& symbol, std::int64_t target_qty, Cents px, Cents costs, Date session, const std::string& ts_utc, const EntryMeta& meta);

    // ---- marks / rollover ----
    void mark(const std::string& symbol, Cents last_px_cents);
    void mark_all_from(const MarketStore& mkt, Date session);
    // Call after marks at every close. Updates HWMs, sessions held, equity history, drawdown, vol.
    void end_of_session(Date session);
    // Day-start snapshot for daily P&L (call at the first event of a session).
    void start_session(Date session);

    // ---- metadata updates ----
    void set_stop(const std::string& symbol, Cents stop_px_cents, std::optional<std::string> broker_order_id = std::nullopt);
    void mark_scaled_down(const std::string& symbol);
    void update_next_report_date(const std::string& symbol, Date next_report, const TradingCalendar& cal, int drift_window, int exit_sessions_before);
    // Live: replace qty/avg_px with broker truth, keep strategy metadata; drop positions the broker no longer has;
    // add unknown ones with empty metadata (flagged unprotected by the reconciler).
    void sync_from_broker(const std::vector<PositionState>& broker_positions, Date session, const std::string& ts_utc);
    void set_cash(Cents cash) { cash_ = cash; }
    // TSMOM only: the broker's Reg-T marginable buying power (Alpaca's plain "buying_power",
    // distinct from the non_marginable figure the earnings strategy sizes against), needed
    // because shorting requires a margin account. 0 means "not synced yet" -- callers treat
    // that as "unknown," never as "zero capacity" (same convention as buying_power_cents).
    void set_margin_buying_power(Cents c) { margin_buying_power_ = c; }

    // ---- views ----
    Cents cash() const { return cash_; }
    Cents equity() const;
    Cents gross_exposure() const;
    Cents hwm_equity() const { return hwm_equity_; }
    double drawdown_pct() const;
    double daily_pnl_pct() const;
    const std::vector<PositionState>& positions() const { return positions_; }
    std::optional<PositionState> position(const std::string& symbol) const;
    std::map<std::string, double> sector_exposure_pct() const;
    std::map<std::string, double> asset_class_exposure_pct() const;
    const std::vector<ClosedTrade>& trades() const { return trades_; }
    const std::vector<DailyRecord>& daily() const { return daily_; }
    int consecutive_losers() const { return consecutive_losers_; }
    std::optional<double> realized_vol_annual(int lookback = 20) const; // fraction
    Cents initial_cash() const { return initial_cash_; }
    Cents total_costs() const { return total_costs_; }

    nlohmann::json portfolio_state_payload(const std::string& as_of_utc, Date session, bool reconciled, const nlohmann::json& open_orders,
                                           int new_positions_today, int orders_today, int rejects_today, std::optional<bool> spy_above_trend) const;

    nlohmann::json state_json() const;
    void load_state(const nlohmann::json& j);

private:
    PositionState* find(const std::string& symbol);
    // Records a brand-new position (flat -> nonzero), used by apply_rebalance_fill both for
    // a plain open and for the "open" half of a zero-crossing flip.
    void open_rebalance_position(const std::string& symbol, std::int64_t qty, Cents px, Cents costs, Date session, const std::string& ts_utc, const EntryMeta& meta);
    Cents initial_cash_;
    Cents cash_;
    std::vector<PositionState> positions_;
    std::vector<ClosedTrade> trades_;
    std::vector<DailyRecord> daily_;
    Cents hwm_equity_;
    Cents day_start_equity_;
    Date day_start_session_{};
    Cents total_costs_ = 0;
    Cents margin_buying_power_ = 0;
    int consecutive_losers_ = 0;
    // per-position cost basis for P&L on partial exits
    std::map<std::string, Cents> entry_costs_;
    // entry metadata carried into the ClosedTrade record
    std::map<std::string, ClosedTrade> open_meta_;
};

} // namespace at
