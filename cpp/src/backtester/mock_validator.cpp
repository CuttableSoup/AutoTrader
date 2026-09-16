#include "backtester/mock_validator.hpp"

#include <cmath>

namespace at {

MockValidatorConfig MockValidatorConfig::from_json(const nlohmann::json& j) {
    MockValidatorConfig c;
    if (j.contains("reject_revenue_miss_with_large_ear")) c.reject_revenue_miss_with_large_ear = j["reject_revenue_miss_with_large_ear"].get<bool>();
    if (j.contains("large_ear_pct")) c.large_ear_pct = j["large_ear_pct"].get<double>();
    if (j.contains("reject_second_8k_in_window")) c.reject_second_8k_in_window = j["reject_second_8k_in_window"].get<bool>();
    if (j.contains("reject_counter_gap_atr_mult")) c.reject_counter_gap_atr_mult = j["reject_counter_gap_atr_mult"].get<double>();
    if (j.contains("counter_gap_window_sessions")) c.counter_gap_window_sessions = j["counter_gap_window_sessions"].get<int>();
    if (j.contains("reject_over_optioned")) c.reject_over_optioned = j["reject_over_optioned"].get<bool>();
    return c;
}

nlohmann::json MockVerdict::to_payload(const std::string& candidate_msg_id, const std::string& symbol, const std::string& mode) const {
    return {
        {"candidate_msg_id", candidate_msg_id}, {"symbol", symbol}, {"verdict", verdict},
        {"confidence", verdict == "APPROVE" ? 0.5 : 0.9}, {"reasons", reasons}, {"flags", flags}, {"model", "mock"},
        {"latency_ms", 0}, {"cost_usd", 0.0}, {"citations", nlohmann::json::array()}, {"mode", mode},
        {"prompt_version", used_lookahead ? "mock-v1-lookahead-proxy" : "mock-v1"},
    };
}

MockValidator::MockValidator(MockValidatorConfig cfg, std::set<std::string> over_optioned, const TradingCalendar& cal)
    : cfg_(std::move(cfg)), over_optioned_(std::move(over_optioned)), cal_(cal) {}

MockVerdict MockValidator::validate(const Candidate& c, const EarningsEvent* ev, const MarketStore& mkt, Date entry_session, int drift_window) const {
    MockVerdict v;
    auto reject = [&](const char* flag, std::string reason, bool lookahead = false) {
        v.verdict = "REJECT";
        v.flags.push_back(flag);
        v.reasons.push_back(std::move(reason));
        if (lookahead) v.used_lookahead = true;
    };

    if (cfg_.reject_over_optioned && over_optioned_.count(c.symbol)) reject("OVER_OPTIONED", "symbol is in the over-optioned exclusion set");

    if (ev && cfg_.reject_revenue_miss_with_large_ear && ev->revenue_actual_cents && ev->revenue_consensus_cents &&
        *ev->revenue_actual_cents <= *ev->revenue_consensus_cents && std::fabs(c.ear_pct) >= cfg_.large_ear_pct)
        reject("REVENUE_MISS", "revenue at or below consensus while |EAR| >= " + std::to_string(cfg_.large_ear_pct) + "% (one-off item proxy)");

    if (ev && cfg_.reject_second_8k_in_window && !ev->material_8k_dates.empty()) {
        Date window_end = cal_.add_sessions(entry_session, drift_window);
        for (Date d : ev->material_8k_dates)
            if (d > ev->report_date && d <= window_end) { reject("SECOND_8K_IN_WINDOW", "material 8-K on " + iso_date(d) + " inside the drift window", true); break; }
    }

    if (cfg_.reject_counter_gap_atr_mult > 0 && c.atr20_cents > 0) {
        const BarSeries* s = mkt.bars(c.symbol);
        if (s) {
            std::size_t i = lower_bound_date(*s, entry_session);
            Cents threshold = static_cast<Cents>(std::llround(static_cast<double>(c.atr20_cents) * cfg_.reject_counter_gap_atr_mult));
            for (int k = 0; k < cfg_.counter_gap_window_sessions && i + static_cast<std::size_t>(k) < s->size(); ++k) {
                std::size_t idx = i + static_cast<std::size_t>(k);
                if (idx == 0) continue;
                Cents gap = (*s)[idx - 1].close - (*s)[idx].open;
                if (gap > threshold) { reject("COUNTER_GAP", "gap down of " + cents_to_decimal(gap) + " on " + iso_date((*s)[idx].date) + " exceeds " + std::to_string(cfg_.reject_counter_gap_atr_mult) + "x ATR", true); break; }
            }
        }
    }
    if (v.verdict == "APPROVE") v.reasons.push_back("no mock rule triggered");
    return v;
}

} // namespace at
