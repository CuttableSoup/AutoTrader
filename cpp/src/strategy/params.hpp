// Frozen strategy parameters (config/strategy.v1.json). Only
// signal.tunable.{ear_threshold_pct, momentum_top_pct} may be changed by the
// walk-forward tuner; the backtester enforces that.
#pragma once
#include "common/config.hpp"
#include "common/money.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace at {

struct UniverseParams {
    Cents min_market_cap_cents = 500000000000LL;     // $5B
    Cents min_adv20_dollars_cents = 5000000000LL;     // $50M
    double max_median_spread_bps = 5.0;
    int min_analyst_coverage = 5;
    bool require_transcript = true;
    int min_listing_age_days = 365;
    bool exclude_etfs = true;
    bool exclude_adrs = true;
    std::set<std::string> exclude_over_optioned;
    std::string benchmark = "SPY";
};

struct SignalParams {
    std::string ear_window = "CLOSE_DM1_TO_CLOSE_DP1";
    std::string ear_benchmark = "SPY";
    bool ear_top_tercile_fallback = true;
    double volume_multiple = 2.0;
    int adv_window_sessions = 20;
    int momentum_lookback_sessions = 252;
    int momentum_skip_sessions = 21;
    int trend_ma_sessions = 200;
    std::string trend_symbol = "SPY";
    int entry_max_sessions_after_signal = 2;
    bool pullback_rsi_enabled = false;
    int pullback_rsi_period = 5;
    double pullback_rsi_max = 50.0;
    bool revision_breadth_enabled = false;
    double revision_min_breadth = 0.3;
    int skip_entry_days_before_ex_dividend = 2;
    // tunable
    double ear_threshold_pct = 3.0;
    double momentum_top_pct = 40.0;
    std::vector<double> grid_ear_threshold_pct{2.0, 3.0, 4.0, 5.0};
    std::vector<double> grid_momentum_top_pct{30.0, 40.0, 50.0};
};

struct ExitParams {
    int drift_window_sessions = 40;
    int exit_sessions_before_earnings = 1;
    int atr_period = 20;
    double trailing_stop_atr_mult = 3.0;
    double trend_break_scale_down_pct = 50.0;
    std::optional<double> take_profit_atr_mult;
};

struct SizingParams {
    double target_vol_annual_pct = 11.0;
    double target_vol_band_lo_pct = 10.0;
    double target_vol_band_hi_pct = 12.0;
    bool inverse_vol_tilt = true;
    int vol_lookback_sessions = 20;
    int max_positions = 15;
    double single_name_cap_pct = 8.0;
    double sector_cap_pct = 30.0;
    double gap_atr_mult = 2.0;
    double gap_max_equity_pct = 0.5;
};

struct OrderParams {
    std::string order_type = "oto";   // oto | bracket
    std::string tif = "gtc";
    double entry_limit_offset_bps = 20.0;
};

struct StrategyParams {
    std::string strategy_version = "EARNINGS_MOMENTUM_V1.0";
    UniverseParams universe;
    SignalParams signal;
    ExitParams exit;
    SizingParams sizing;
    OrderParams order;

    static StrategyParams from_json(const nlohmann::json& j);
    static StrategyParams load(const std::filesystem::path& file);
    nlohmann::json to_json() const;
    // Identifier of this exact parameter set (for reports and candidate metadata).
    std::string fingerprint() const;
};

} // namespace at
