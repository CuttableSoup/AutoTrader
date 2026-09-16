// v1.0 earnings-momentum signal (docs/DESIGN.md 2.2), evaluated at the close
// of session day0+1. Pure functions over MarketStore so replay and live agree.
#pragma once
#include "strategy/market_store.hpp"
#include "strategy/params.hpp"
#include "strategy/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace at {

struct SignalContext {
    const MarketStore& mkt;
    const UniverseSnapshot& universe;
    const TradingCalendar& cal;
    Date session;                       // the session whose close we are evaluating
    std::string data_as_of_utc;
    // Distribution of 12-1 momentum across the universe at `session`, for percentile ranks.
    std::vector<double> universe_momentum;
    // EAR cutoff for the "top tercile of this week's announcers" fallback, if known.
    std::optional<double> ear_tercile_cutoff_pct;
};

struct EarComputation {
    bool ok = false;
    Date day0{};
    Date day_minus1{};
    Date day_plus1{};
    double raw_return_pct = 0;
    double benchmark_return_pct = 0;
    double ear_pct = 0;
    std::int64_t day0_volume = 0;
    double adv20_shares = 0;
    double vol_ratio = 0;
    std::string error;
};

// Close(day0-1) -> close(day0+1) for the symbol minus the same for the benchmark.
EarComputation compute_ear(const EarningsEvent& ev, const MarketStore& mkt, const TradingCalendar& cal, const SignalParams& p);

// Is SPY (trend_symbol) above its trend SMA at the close of `session`?
std::optional<bool> trend_filter(const MarketStore& mkt, Date session, const SignalParams& p);

// 12-1 momentum values for every universe member with enough history.
std::vector<double> universe_momentum_distribution(const MarketStore& mkt, const UniverseSnapshot& u, Date session, const SignalParams& p);

struct SignalEvaluation {
    bool passed = false;
    std::vector<std::string> failed;       // condition names that failed
    std::vector<std::string> checked;      // "name=value" for logs
    std::optional<Candidate> candidate;    // set when passed
    EarComputation ear;
};

// Evaluate one earnings event at ctx.session. The event's day0+1 must equal ctx.session
// (otherwise failed = ["not_signal_session"]).
SignalEvaluation evaluate_earnings_signal(const EarningsEvent& ev, const SignalContext& ctx, const StrategyParams& params);

// Neutral thesis facts for the validator. Numbers only, no adjectives.
std::vector<std::string> build_thesis_facts(const EarningsEvent& ev, const EarComputation& ear, double mom_pct, std::optional<Date> next_report);

} // namespace at
