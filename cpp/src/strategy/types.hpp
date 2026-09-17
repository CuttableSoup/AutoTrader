// Domain types shared by strategy, risk, portfolio and the backtester.
#pragma once
#include "common/calendar.hpp"
#include "common/indicators.hpp"
#include "common/money.hpp"
#include "common/time.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace at {

// Point-in-time security metadata (Sharadar TICKERS/SF1 in backtests, live vendors otherwise).
struct SecurityInfo {
    std::string symbol;
    std::string name;
    std::string sector;
    std::string category;         // "Domestic Common Stock", "ETF", "ADR Common Stock", ...
    std::string asset_class;      // TSMOM only: "EQUITY","RATES_CREDIT","COMMODITIES","CURRENCIES"
    Cents market_cap_cents = 0;
    std::optional<Date> first_listed;
    int analyst_coverage = 0;
    bool transcript_available = false;
    double median_spread_bps = 0.0;
    Date as_of{};

    bool is_etf() const;
    bool is_adr() const;
    bool is_common_stock() const;
};

struct UniverseSnapshot {
    std::string id;                 // sha256(as_of|symbols)[0:16]
    Date as_of{};
    std::vector<std::string> symbols;
    std::unordered_map<std::string, SecurityInfo> info;
    std::unordered_map<std::string, std::string> exclusion_reasons; // symbol -> first failed rule
    bool contains(const std::string& s) const { return info.count(s) > 0; }
    std::string sector_of(const std::string& s) const;
};

struct EarningsEvent {
    enum class Timing { BMO, AMC, UNKNOWN };
    std::string event_id;
    std::string symbol;
    Date report_date{};
    Timing timing = Timing::UNKNOWN;
    std::string fiscal_period;
    std::optional<double> eps_actual;
    std::optional<double> eps_consensus;
    std::optional<Date> eps_consensus_asof;
    std::optional<Cents> revenue_actual_cents;
    std::optional<Cents> revenue_consensus_cents;
    std::optional<Date> next_report_date;
    std::vector<Date> material_8k_dates;
    std::string source;

    // First session that can react: report_date if BMO and a trading day; otherwise the next session.
    Date day0(const TradingCalendar& cal) const;
    bool has_actuals() const { return eps_actual.has_value(); }
    static Timing parse_timing(std::string_view s);
    static const char* timing_str(Timing t);
    static EarningsEvent from_json(const nlohmann::json& payload);
    nlohmann::json to_json() const;
};

struct Quote {
    Cents bid_cents = 0;
    Cents ask_cents = 0;
    std::int64_t bid_size = 0;
    std::int64_t ask_size = 0;
    SysTime ts{};
    bool halted = false;
    Cents mid() const { return (bid_cents + ask_cents) / 2; }
    double spread_bps() const { return mid() == 0 ? 0.0 : static_cast<double>(ask_cents - bid_cents) / static_cast<double>(mid()) * 10000.0; }
};

// Mirrors schemas/v1/signals.candidate.schema.json.
struct Candidate {
    std::string symbol;
    std::string side = "BUY";
    std::string signal_type = "EARNINGS_MOMENTUM_V1";
    std::string strategy_version;
    std::string event_id;
    Date session_date{};
    double ear_pct = 0;
    double vol_ratio = 0;
    double mom_pct = 0;
    std::optional<double> rsi5;
    std::optional<double> revision_breadth;
    Cents entry_px_ref_cents = 0;
    Cents atr20_cents = 0;
    std::int64_t adv20_shares = 0;
    std::string sector;
    bool spy_above_trend = false;
    Date entry_deadline_date{};
    std::optional<Date> next_report_date;
    std::string universe_snapshot_id;
    std::vector<std::string> thesis_facts;
    std::string data_as_of_utc;
    // TSMOM_ETF_V1 only.
    std::optional<int> mom_sign;          // sign(12m total return) in {-1,0,1}
    std::optional<double> vol_annual_pct; // trailing 60-session annualized vol, percent
    std::optional<double> target_weight_pct; // signed, -100..100
    std::string asset_class;

    nlohmann::json to_json() const;
    static Candidate from_json(const nlohmann::json& payload);
};

// Position with strategy metadata (mirrors portfolio.state.positions[]).
struct PositionState {
    std::string symbol;
    std::int64_t qty = 0;
    Cents avg_px_cents = 0;
    Cents last_px_cents = 0;
    std::string entry_ts_utc;
    Date entry_session{};
    int sessions_held = 0;
    Date exit_deadline{};
    std::optional<Date> next_report_date;
    std::optional<Cents> stop_px_cents;
    std::optional<std::string> stop_broker_order_id;
    std::optional<Cents> hwm_px_cents;
    std::optional<Cents> atr20_cents;
    std::string sector;
    std::string asset_class;    // TSMOM only: also identifies which strategy owns this position
    std::optional<std::string> candidate_msg_id;
    bool scaled_down = false;

    Cents market_value_cents() const { return qty * last_px_cents; }
    Cents unrealized_cents() const { return qty * (last_px_cents - avg_px_cents); }
    nlohmann::json to_json() const;
    static PositionState from_json(const nlohmann::json& j);
};

} // namespace at
