#include "risk/limits.hpp"

#include <fstream>
#include <stdexcept>

namespace at {

namespace {
template <typename T>
void rd(const nlohmann::json& j, const char* k, T& out) {
    if (j.contains(k) && !j[k].is_null()) out = j[k].get<T>();
}
} // namespace

RiskLimits RiskLimits::from_json(const nlohmann::json& j) {
    RiskLimits r;
    rd(j, "risk_version", r.risk_version);
    rd(j, "per_position_risk_pct", r.per_position_risk_pct);
    rd(j, "single_name_cap_pct", r.single_name_cap_pct);
    rd(j, "sector_cap_pct", r.sector_cap_pct);
    rd(j, "asset_class_cap_pct", r.asset_class_cap_pct);
    rd(j, "gross_exposure_cap_pct", r.gross_exposure_cap_pct);
    rd(j, "max_open_positions", r.max_open_positions);
    rd(j, "max_new_positions_per_day", r.max_new_positions_per_day);
    rd(j, "max_orders_per_day", r.max_orders_per_day);
    rd(j, "daily_loss_pause_pct", r.daily_loss_pause_pct);
    rd(j, "drawdown_halve_pct", r.drawdown_halve_pct);
    rd(j, "drawdown_flatten_pct", r.drawdown_flatten_pct);
    rd(j, "consecutive_losers_pause", r.consecutive_losers_pause);
    rd(j, "max_spread_bps", r.max_spread_bps);
    rd(j, "max_order_pct_of_adv20", r.max_order_pct_of_adv20);
    rd(j, "earnings_gate", r.earnings_gate);
    rd(j, "max_quote_age_s", r.max_quote_age_s);
    rd(j, "max_candidate_age_sessions", r.max_candidate_age_sessions);
    rd(j, "validator_error_streak_pause", r.validator_error_streak_pause);
    rd(j, "reject_rate_page_pct", r.reject_rate_page_pct);
    rd(j, "shadow_mode", r.shadow_mode);
    if (r.gross_exposure_cap_pct > 100.0) throw std::runtime_error("risk limits: gross exposure > 100% is not allowed in v1 (no margin)");
    if (r.drawdown_flatten_pct >= r.drawdown_halve_pct) throw std::runtime_error("risk limits: drawdown_flatten_pct must be below drawdown_halve_pct");
    return r;
}

RiskLimits RiskLimits::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("risk limits: cannot open " + file.string());
    nlohmann::json j;
    in >> j;
    return from_json(j);
}

nlohmann::json RiskLimits::to_json() const {
    return {
        {"risk_version", risk_version},
        {"per_position_risk_pct", per_position_risk_pct},
        {"single_name_cap_pct", single_name_cap_pct},
        {"sector_cap_pct", sector_cap_pct},
        {"asset_class_cap_pct", asset_class_cap_pct},
        {"gross_exposure_cap_pct", gross_exposure_cap_pct},
        {"max_open_positions", max_open_positions},
        {"max_new_positions_per_day", max_new_positions_per_day},
        {"max_orders_per_day", max_orders_per_day},
        {"daily_loss_pause_pct", daily_loss_pause_pct},
        {"drawdown_halve_pct", drawdown_halve_pct},
        {"drawdown_flatten_pct", drawdown_flatten_pct},
        {"consecutive_losers_pause", consecutive_losers_pause},
        {"max_spread_bps", max_spread_bps},
        {"max_order_pct_of_adv20", max_order_pct_of_adv20},
        {"earnings_gate", earnings_gate},
        {"max_quote_age_s", max_quote_age_s},
        {"max_candidate_age_sessions", max_candidate_age_sessions},
        {"validator_error_streak_pause", validator_error_streak_pause},
        {"reject_rate_page_pct", reject_rate_page_pct},
        {"shadow_mode", shadow_mode},
    };
}

} // namespace at
