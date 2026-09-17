// Strategy engine: holds market data, the universe and known earnings events;
// at every session close evaluates the v1 signal and returns candidates.
// Used unchanged by the live service and the backtester.
#pragma once
#include "strategy/market_store.hpp"
#include "strategy/params.hpp"
#include "strategy/signal.hpp"
#include "strategy/tsmom_params.hpp"
#include "strategy/tsmom_signal.hpp"
#include "strategy/types.hpp"

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace at {

class StrategyEngine {
public:
    StrategyEngine(StrategyParams params, const TradingCalendar& cal);

    MarketStore& market() { return mkt_; }
    const MarketStore& market() const { return mkt_; }
    const StrategyParams& params() const { return params_; }
    void set_params(StrategyParams p) { params_ = std::move(p); }
    void set_tsmom_params(TsmomParams p) { tsmom_params_ = std::move(p); }

    void set_universe(UniverseSnapshot u) { universe_ = std::move(u); }
    const UniverseSnapshot& universe() const { return universe_; }

    void on_bar(const std::string& symbol, const Bar& bar) { mkt_.add_bar(symbol, bar); }
    // Upsert by event_id (scheduled first, actuals later).
    void on_earnings_event(const EarningsEvent& ev);
    const std::map<std::string, EarningsEvent>& events() const { return events_; }
    // Next scheduled report date for a symbol strictly after `after`, if known.
    std::optional<Date> next_report_date(const std::string& symbol, Date after) const;

    struct SessionResult {
        std::vector<Candidate> candidates;
        std::vector<SignalEvaluation> evaluations;   // every event examined, for logs/backtest stats
        std::optional<bool> spy_above_trend;
    };
    // Close-of-session evaluation. Idempotent per event_id: an event yields at most one candidate.
    SessionResult evaluate_session(Date session, const std::string& data_as_of_utc);

    // Events whose signal session (day0+1) is `session`.
    std::vector<const EarningsEvent*> events_for_signal_session(Date session) const;

    struct RebalanceSessionResult {
        std::vector<Candidate> candidates;
        std::vector<TsmomInstrumentSignal> signals;   // every universe member, for logs/backtest stats
    };
    // TSMOM monthly rebalance evaluation. No-ops (empty result) unless `session` is a
    // formation session (tsmom_signal.hpp's is_formation_session). Idempotent per
    // (universe_id, symbol, month) -- not events_-keyed, since there's no EarningsEvent
    // involved. This engine instance must hold the TSMOM 18-ETF universe (set_universe
    // with build_tsmom_universe) and nothing else -- TSMOM runs as a separate service
    // with its own StrategyEngine instance rather than sharing at_strategy_svc's, so a
    // single UniverseSnapshot member never needs to serve two universes at once.
    RebalanceSessionResult evaluate_rebalance_session(Date session, const std::string& data_as_of_utc);

private:
    StrategyParams params_;
    TsmomParams tsmom_params_;
    const TradingCalendar& cal_;
    MarketStore mkt_;
    UniverseSnapshot universe_;
    std::map<std::string, EarningsEvent> events_;
    std::set<std::string> emitted_event_ids_;
    std::set<std::string> emitted_tsmom_keys_;   // "{universe_id}|{symbol}|{yyyy-mm}"
    std::deque<std::pair<Date, double>> recent_ears_;   // (signal session, ear) for the tercile fallback
};

} // namespace at
