// Deterministic stand-in for the Claude validator (docs/DESIGN.md 7.1).
// Two of its rules (counter-gap, second 8-K) peek at data after the signal.
// They are proxies for what a real validator might learn from the news and
// are labelled lookahead in every verdict so nobody mistakes them for edge.
#pragma once
#include "common/calendar.hpp"
#include "strategy/market_store.hpp"
#include "strategy/types.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <vector>

namespace at {

struct MockValidatorConfig {
    bool reject_revenue_miss_with_large_ear = true;
    double large_ear_pct = 8.0;
    bool reject_second_8k_in_window = true;
    double reject_counter_gap_atr_mult = 1.5;   // <= 0 disables
    int counter_gap_window_sessions = 3;
    bool reject_over_optioned = true;
    static MockValidatorConfig from_json(const nlohmann::json& j);
};

struct MockVerdict {
    std::string verdict = "APPROVE";          // APPROVE | REJECT
    std::vector<std::string> flags;
    std::vector<std::string> reasons;
    bool used_lookahead = false;
    nlohmann::json to_payload(const std::string& candidate_msg_id, const std::string& symbol, const std::string& mode) const;
};

class MockValidator {
public:
    MockValidator(MockValidatorConfig cfg, std::set<std::string> over_optioned, const TradingCalendar& cal);
    // entry_session: the session the entry would fill at the open of. drift_window: sessions.
    MockVerdict validate(const Candidate& c, const EarningsEvent* ev, const MarketStore& mkt, Date entry_session, int drift_window) const;
    const MockValidatorConfig& config() const { return cfg_; }

private:
    MockValidatorConfig cfg_;
    std::set<std::string> over_optioned_;
    const TradingCalendar& cal_;
};

} // namespace at
