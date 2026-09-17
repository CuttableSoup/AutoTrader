// Risk manager: the only producer of orders.approved. Holds pending candidates
// until the next open, applies every hard limit from risk.v1.json, sizes the
// position, and runs the end-of-session exit sweep. Emits control.* on
// breaches. Pure logic; the service wrapper does bus IO and state persistence.
#pragma once
#include "common/bus.hpp"
#include "common/calendar.hpp"
#include "risk/limits.hpp"
#include "strategy/params.hpp"
#include "strategy/sizing.hpp"
#include "strategy/tsmom_params.hpp"
#include "strategy/tsmom_sizing.hpp"
#include "strategy/types.hpp"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace at {

struct RiskCheck {
    std::string name;
    bool ok = true;
    nlohmann::json value = nullptr;
    nlohmann::json limit = nullptr;
    nlohmann::json to_json() const { return {{"name", name}, {"ok", ok}, {"value", value}, {"limit", limit}}; }
};

struct PendingCandidate {
    std::string msg_id;
    Candidate cand;
    std::optional<std::string> verdict;           // APPROVE / REJECT / ERROR
    std::optional<std::string> validated_msg_id;
    std::string received_utc;
};

struct EntryDecision {
    bool approved = false;
    std::string candidate_msg_id;
    std::string symbol;
    std::string reject_reason;
    std::vector<RiskCheck> checks;
    SizingResult sizing;
    nlohmann::json order;   // orders.approved payload when approved
};

struct PortfolioView {
    bool valid = false;
    bool reconciled = false;
    std::string as_of_utc;
    Date session{};
    Cents equity_cents = 0;
    Cents cash_cents = 0;
    Cents buying_power_cents = 0;
    Cents margin_buying_power_cents = 0;   // TSMOM only: Alpaca's Reg-T marginable buying power
    Cents gross_exposure_cents = 0;
    std::map<std::string, double> sector_exposure_pct;
    std::map<std::string, double> asset_class_exposure_pct;   // TSMOM only, gross per bucket
    Cents hwm_equity_cents = 0;
    double drawdown_pct = 0;
    double daily_pnl_pct = 0;
    int consecutive_losers = 0;
    int closed_trades_total = 0;
    std::optional<bool> spy_above_trend;
    std::optional<double> realized_vol_annual;   // fraction
    std::vector<PositionState> positions;
    int open_orders = 0;
};

struct RiskCounters {
    Date session{};
    int new_positions_today = 0;
    int orders_today = 0;
    int rejects_today = 0;          // broker rejects
    int submitted_today = 0;
    int validator_missing_streak = 0;
    int validator_error_streak = 0;
};

class RiskManager {
public:
    // tp is set only by services that also run the TSMOM book (at_risk_svc when
    // tsmom_config is configured, and the backtester's TSMOM harness); the earnings-only
    // path never reads it.
    RiskManager(StrategyParams sp, RiskLimits rl, const TradingCalendar& cal, std::optional<TsmomParams> tp = std::nullopt);

    // ---- inputs -----------------------------------------------------------
    void on_candidate(const Envelope& env);
    void on_validated(const Envelope& env);
    void on_portfolio_state(const nlohmann::json& payload);
    void on_reconcile(const nlohmann::json& payload);
    void on_control(const std::string& subject, const nlohmann::json& payload);
    void on_quote(const std::string& symbol, const Quote& q);
    void on_shortable(const std::string& symbol, bool shortable);   // TSMOM pre-flight; market.data.shortable.*
    void on_order_status(const nlohmann::json& payload);
    void on_order_filled(const nlohmann::json& payload);

    // ---- decisions --------------------------------------------------------
    // Called once at/after the open of `session` with fresh quotes loaded via on_quote.
    // Returns the decisions (approved and rejected). Approved ones carry the orders.approved payload.
    std::vector<EntryDecision> process_pending_entries(Date session, SysTime now);
    EntryDecision evaluate_entry(const PendingCandidate& pc, Date session, SysTime now);
    // TSMOM: resizes the candidate's symbol to its target weight. No validator wait (TSMOM
    // bypasses the Claude sidecar entirely), no SPY-trend/earnings-gate/max-open-positions
    // checks (this is a rebalance of a fixed 18-name book, not a bounded set of new entries).
    EntryDecision evaluate_rebalance(const PendingCandidate& pc, Date session, SysTime now);

    // Called after the close of `session`. Returns orders.approved payloads for exits / stop ratchets.
    std::vector<nlohmann::json> end_of_session_sweep(Date session);

    // Control messages produced since the last call: (subject, payload).
    std::vector<std::pair<std::string, nlohmann::json>> take_control_messages();

    // Fresh-day rollover (called by the service when the session changes).
    void start_session(Date session);

    // ---- state ------------------------------------------------------------
    bool paused_new() const { return paused_new_; }
    bool halted() const { return halted_; }
    bool size_halved() const { return size_halved_; }
    bool shadow_mode() const { return limits_.shadow_mode; }
    const RiskCounters& counters() const { return counters_; }
    const PortfolioView& portfolio() const { return pf_; }
    std::size_t pending_count() const { return pending_.size(); }
    const std::deque<PendingCandidate>& pending() const { return pending_; }
    nlohmann::json state_json() const;
    void load_state(const nlohmann::json& j);
    const StrategyParams& strategy_params() const { return sp_; }
    const RiskLimits& limits() const { return limits_; }

private:
    void emit_control(const std::string& command, const std::string& trigger, const std::string& reason, nlohmann::json details = nlohmann::json::object());
    void apply_drawdown_rules();
    std::optional<PositionState> position(const std::string& symbol) const;
    Cents sector_exposure_cents(const std::string& sector) const;
    Cents asset_class_exposure_cents(const std::string& asset_class) const;
    nlohmann::json make_exit_order(const PositionState& pos, const std::string& intent, std::int64_t qty, const std::string& reason, Date session, std::optional<Cents> new_stop = std::nullopt);

    StrategyParams sp_;
    std::optional<TsmomParams> tp_;
    RiskLimits limits_;
    const TradingCalendar& cal_;
    PortfolioView pf_;
    RiskCounters counters_;
    std::deque<PendingCandidate> pending_;
    std::map<std::string, Quote> quotes_;
    std::map<std::string, bool> shortable_;   // TSMOM: symbol -> broker-confirmed shortable+easy_to_borrow
    std::vector<std::pair<std::string, nlohmann::json>> control_out_;
    std::map<std::string, std::string> approved_by_symbol_;   // symbol -> client_order_id, entries approved but not yet in portfolio
    Dedupe seen_candidates_{200000};                          // candidate msg_ids ever accepted (bus is at-least-once)
    std::map<std::string, std::pair<std::string, std::string>> early_verdicts_;   // candidate_msg_id -> (verdict, validated_msg_id) that arrived before the candidate
    bool paused_new_ = false;
    std::string pause_reason_;
    bool halted_ = false;
    bool size_halved_ = false;
    bool flatten_issued_ = false;
    bool daily_loss_paused_today_ = false;
    bool reconcile_clean_ = false;
};

} // namespace at
