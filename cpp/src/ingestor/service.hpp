// Market data ingestor: daily bars for universe + held symbols + SPY at the
// configured ET times, and fresh NBBO quotes around the open for symbols the
// risk manager is about to trade. Publishes market.data.bar.* / quote.*.
#pragma once
#include "alpaca/client.hpp"
#include "common/bus.hpp"
#include "common/calendar.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace at {

struct IngestorConfig {
    std::vector<int> bar_pull_minutes_et{9 * 60 + 20, 16 * 60 + 30};
    int bar_lookback_sessions = 300;
    int quote_window_start_minutes_et = 9 * 60 + 30;
    int quote_window_end_minutes_et = 9 * 60 + 45;
    int quote_interval_s = 10;
    std::string trend_symbol = "SPY";
    std::filesystem::path universe_file;   // var/state/universe.json
    bool pull_tsmom_tr_bars = true;        // pull dividend-adjusted bars for the fixed 18-ETF TSMOM universe (strategy/tsmom_universe.hpp)
};

class IngestorService {
public:
    IngestorService(AlpacaClient& client, IBus& bus, IngestorConfig cfg, const TradingCalendar& cal);

    void on_candidate(const Envelope& env);         // symbols that need quotes until their entry deadline
    void on_portfolio_state(const Envelope& env);   // held symbols need bars
    void tick(SysTime now);

    // Pull and publish bars for the given symbols (incremental after the first pull). Returns bars published.
    // Published to market.data.bar.<SYM>, split-adjusted (raw traded prices) -- every existing consumer's assumption.
    std::size_t pull_bars(const std::vector<std::string>& symbols, SysTime now, bool full = false);
    // TSMOM only: the same symbols, dividend+split (total-return) adjusted, published to the
    // separate market.data.bar_tr.<SYM> subject. Kept off market.data.bar.* deliberately: SPY
    // is both the earnings strategy's trend_symbol (needs raw/split-adjusted prices for its
    // SMA-200 filter) and a TSMOM universe member (needs total-return-adjusted prices for its
    // momentum/vol) -- one subject cannot serve both without corrupting one consumer's series.
    std::size_t pull_bars_tr(const std::vector<std::string>& symbols, SysTime now, bool full = false);
    // TSMOM only: refreshes shortable/easy_to_borrow for the fixed universe, once per
    // calendar day (it rarely changes; no need for the bar-pull cadence). Published to
    // market.data.shortable.<SYM> for RiskManager::evaluate_rebalance's pre-flight check.
    std::size_t pull_shortable(const std::vector<std::string>& symbols, SysTime now);
    std::size_t pull_quotes(const std::vector<std::string>& symbols, SysTime now);
    std::vector<std::string> universe_symbols() const;
    std::vector<std::string> all_symbols() const;
    std::vector<std::string> tsmom_symbols() const;

private:
    AlpacaClient& client_;
    IBus& bus_;
    IngestorConfig cfg_;
    const TradingCalendar& cal_;
    std::map<std::string, Date> last_bar_published_;
    std::map<std::string, Date> last_tr_bar_published_;
    std::map<std::string, Date> quote_symbols_;     // symbol -> entry deadline
    std::set<std::string> held_;
    std::map<int, Date> pulled_;                    // pull slot (minutes) -> session pulled
    Date last_shortable_pulled_{};
    SysTime last_quote_{};
};

} // namespace at
