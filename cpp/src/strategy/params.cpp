#include "strategy/params.hpp"

#include "common/sha256.hpp"

#include <fstream>
#include <stdexcept>

namespace at {

namespace {
template <typename T>
void rd(const nlohmann::json& j, const char* k, T& out) {
    if (j.contains(k) && !j[k].is_null()) out = j[k].get<T>();
}
} // namespace

StrategyParams StrategyParams::from_json(const nlohmann::json& j) {
    StrategyParams p;
    rd(j, "strategy_version", p.strategy_version);
    if (j.contains("universe")) {
        const auto& u = j["universe"];
        rd(u, "min_market_cap_cents", p.universe.min_market_cap_cents);
        rd(u, "min_adv20_dollars_cents", p.universe.min_adv20_dollars_cents);
        rd(u, "max_median_spread_bps", p.universe.max_median_spread_bps);
        rd(u, "min_analyst_coverage", p.universe.min_analyst_coverage);
        rd(u, "require_transcript", p.universe.require_transcript);
        rd(u, "min_listing_age_days", p.universe.min_listing_age_days);
        rd(u, "exclude_etfs", p.universe.exclude_etfs);
        rd(u, "exclude_adrs", p.universe.exclude_adrs);
        rd(u, "benchmark", p.universe.benchmark);
        if (u.contains("exclude_over_optioned"))
            for (const auto& s : u["exclude_over_optioned"]) p.universe.exclude_over_optioned.insert(s.get<std::string>());
    }
    if (j.contains("signal")) {
        const auto& s = j["signal"];
        rd(s, "ear_window", p.signal.ear_window);
        rd(s, "ear_benchmark", p.signal.ear_benchmark);
        rd(s, "ear_top_tercile_fallback", p.signal.ear_top_tercile_fallback);
        rd(s, "volume_multiple", p.signal.volume_multiple);
        rd(s, "adv_window_sessions", p.signal.adv_window_sessions);
        rd(s, "momentum_lookback_sessions", p.signal.momentum_lookback_sessions);
        rd(s, "momentum_skip_sessions", p.signal.momentum_skip_sessions);
        rd(s, "trend_ma_sessions", p.signal.trend_ma_sessions);
        rd(s, "trend_symbol", p.signal.trend_symbol);
        rd(s, "entry_max_sessions_after_signal", p.signal.entry_max_sessions_after_signal);
        rd(s, "skip_entry_days_before_ex_dividend", p.signal.skip_entry_days_before_ex_dividend);
        if (s.contains("pullback_rsi")) {
            rd(s["pullback_rsi"], "enabled", p.signal.pullback_rsi_enabled);
            rd(s["pullback_rsi"], "period", p.signal.pullback_rsi_period);
            rd(s["pullback_rsi"], "max_rsi", p.signal.pullback_rsi_max);
        }
        if (s.contains("revision_breadth")) {
            rd(s["revision_breadth"], "enabled", p.signal.revision_breadth_enabled);
            rd(s["revision_breadth"], "min_breadth", p.signal.revision_min_breadth);
        }
        if (s.contains("tunable")) {
            rd(s["tunable"], "ear_threshold_pct", p.signal.ear_threshold_pct);
            rd(s["tunable"], "momentum_top_pct", p.signal.momentum_top_pct);
        }
        if (s.contains("tunable_grid")) {
            rd(s["tunable_grid"], "ear_threshold_pct", p.signal.grid_ear_threshold_pct);
            rd(s["tunable_grid"], "momentum_top_pct", p.signal.grid_momentum_top_pct);
        }
    }
    if (j.contains("exit")) {
        const auto& e = j["exit"];
        rd(e, "drift_window_sessions", p.exit.drift_window_sessions);
        rd(e, "exit_sessions_before_earnings", p.exit.exit_sessions_before_earnings);
        rd(e, "atr_period", p.exit.atr_period);
        rd(e, "trailing_stop_atr_mult", p.exit.trailing_stop_atr_mult);
        rd(e, "trend_break_scale_down_pct", p.exit.trend_break_scale_down_pct);
        if (e.contains("take_profit_atr_mult") && !e["take_profit_atr_mult"].is_null())
            p.exit.take_profit_atr_mult = e["take_profit_atr_mult"].get<double>();
    }
    if (j.contains("sizing")) {
        const auto& s = j["sizing"];
        rd(s, "target_vol_annual_pct", p.sizing.target_vol_annual_pct);
        if (s.contains("target_vol_band_pct") && s["target_vol_band_pct"].is_array() && s["target_vol_band_pct"].size() == 2) {
            p.sizing.target_vol_band_lo_pct = s["target_vol_band_pct"][0].get<double>();
            p.sizing.target_vol_band_hi_pct = s["target_vol_band_pct"][1].get<double>();
        }
        rd(s, "inverse_vol_tilt", p.sizing.inverse_vol_tilt);
        rd(s, "vol_lookback_sessions", p.sizing.vol_lookback_sessions);
        rd(s, "max_positions", p.sizing.max_positions);
        rd(s, "single_name_cap_pct", p.sizing.single_name_cap_pct);
        rd(s, "sector_cap_pct", p.sizing.sector_cap_pct);
        if (s.contains("gap_budget")) {
            rd(s["gap_budget"], "atr_mult", p.sizing.gap_atr_mult);
            rd(s["gap_budget"], "max_equity_pct", p.sizing.gap_max_equity_pct);
        }
    }
    if (j.contains("order")) {
        const auto& o = j["order"];
        rd(o, "order_type", p.order.order_type);
        rd(o, "tif", p.order.tif);
        rd(o, "entry_limit_offset_bps", p.order.entry_limit_offset_bps);
    }
    if (p.order.order_type != "oto" && p.order.order_type != "bracket")
        throw std::runtime_error("strategy params: order.order_type must be oto or bracket");
    if (p.order.order_type == "bracket" && !p.exit.take_profit_atr_mult)
        throw std::runtime_error("strategy params: bracket order_type requires exit.take_profit_atr_mult");
    return p;
}

StrategyParams StrategyParams::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("strategy params: cannot open " + file.string());
    nlohmann::json j;
    in >> j;
    return from_json(j);
}

nlohmann::json StrategyParams::to_json() const {
    nlohmann::json j;
    j["strategy_version"] = strategy_version;
    j["universe"] = {
        {"min_market_cap_cents", universe.min_market_cap_cents},
        {"min_adv20_dollars_cents", universe.min_adv20_dollars_cents},
        {"max_median_spread_bps", universe.max_median_spread_bps},
        {"min_analyst_coverage", universe.min_analyst_coverage},
        {"require_transcript", universe.require_transcript},
        {"min_listing_age_days", universe.min_listing_age_days},
        {"exclude_etfs", universe.exclude_etfs},
        {"exclude_adrs", universe.exclude_adrs},
        {"exclude_over_optioned", std::vector<std::string>(universe.exclude_over_optioned.begin(), universe.exclude_over_optioned.end())},
        {"benchmark", universe.benchmark},
    };
    j["signal"] = {
        {"ear_window", signal.ear_window},
        {"ear_benchmark", signal.ear_benchmark},
        {"ear_top_tercile_fallback", signal.ear_top_tercile_fallback},
        {"volume_multiple", signal.volume_multiple},
        {"adv_window_sessions", signal.adv_window_sessions},
        {"momentum_lookback_sessions", signal.momentum_lookback_sessions},
        {"momentum_skip_sessions", signal.momentum_skip_sessions},
        {"trend_ma_sessions", signal.trend_ma_sessions},
        {"trend_symbol", signal.trend_symbol},
        {"entry_max_sessions_after_signal", signal.entry_max_sessions_after_signal},
        {"pullback_rsi", {{"enabled", signal.pullback_rsi_enabled}, {"period", signal.pullback_rsi_period}, {"max_rsi", signal.pullback_rsi_max}}},
        {"revision_breadth", {{"enabled", signal.revision_breadth_enabled}, {"min_breadth", signal.revision_min_breadth}}},
        {"skip_entry_days_before_ex_dividend", signal.skip_entry_days_before_ex_dividend},
        {"tunable", {{"ear_threshold_pct", signal.ear_threshold_pct}, {"momentum_top_pct", signal.momentum_top_pct}}},
        {"tunable_grid", {{"ear_threshold_pct", signal.grid_ear_threshold_pct}, {"momentum_top_pct", signal.grid_momentum_top_pct}}},
    };
    j["exit"] = {
        {"drift_window_sessions", exit.drift_window_sessions},
        {"exit_sessions_before_earnings", exit.exit_sessions_before_earnings},
        {"atr_period", exit.atr_period},
        {"trailing_stop_atr_mult", exit.trailing_stop_atr_mult},
        {"trend_break_scale_down_pct", exit.trend_break_scale_down_pct},
        {"take_profit_atr_mult", exit.take_profit_atr_mult ? nlohmann::json(*exit.take_profit_atr_mult) : nlohmann::json(nullptr)},
    };
    j["sizing"] = {
        {"target_vol_annual_pct", sizing.target_vol_annual_pct},
        {"target_vol_band_pct", {sizing.target_vol_band_lo_pct, sizing.target_vol_band_hi_pct}},
        {"inverse_vol_tilt", sizing.inverse_vol_tilt},
        {"vol_lookback_sessions", sizing.vol_lookback_sessions},
        {"max_positions", sizing.max_positions},
        {"single_name_cap_pct", sizing.single_name_cap_pct},
        {"sector_cap_pct", sizing.sector_cap_pct},
        {"gap_budget", {{"atr_mult", sizing.gap_atr_mult}, {"max_equity_pct", sizing.gap_max_equity_pct}}},
    };
    j["order"] = {{"order_type", order.order_type}, {"tif", order.tif}, {"entry_limit_offset_bps", order.entry_limit_offset_bps}};
    return j;
}

std::string StrategyParams::fingerprint() const { return sha256_hex(to_json().dump()).substr(0, 16); }

} // namespace at
