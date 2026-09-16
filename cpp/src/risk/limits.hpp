// Hard risk limits (config/risk.v1.json, docs/DESIGN.md section 5).
#pragma once
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace at {

struct RiskLimits {
    std::string risk_version = "RISK_V1.0";
    double per_position_risk_pct = 0.5;
    double single_name_cap_pct = 8.0;
    double sector_cap_pct = 30.0;
    double gross_exposure_cap_pct = 100.0;
    int max_open_positions = 15;
    int max_new_positions_per_day = 3;
    int max_orders_per_day = 30;
    double daily_loss_pause_pct = -2.0;
    double drawdown_halve_pct = -8.0;
    double drawdown_flatten_pct = -12.0;
    int consecutive_losers_pause = 8;
    double max_spread_bps = 10.0;
    double max_order_pct_of_adv20 = 1.0;
    bool earnings_gate = true;
    int max_quote_age_s = 30;
    int max_candidate_age_sessions = 1;
    int validator_error_streak_pause = 3;
    double reject_rate_page_pct = 10.0;
    bool shadow_mode = true;

    static RiskLimits from_json(const nlohmann::json& j);
    static RiskLimits load(const std::filesystem::path& file);
    nlohmann::json to_json() const;
};

} // namespace at
