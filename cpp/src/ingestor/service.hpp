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
    std::filesystem::path universe_file;     // var/state/universe.json
};

class IngestorService {
public:
    IngestorService(AlpacaClient& client, IBus& bus, IngestorConfig cfg, const TradingCalendar& cal);

    void on_candidate(const Envelope& env);         // symbols that need quotes until their entry deadline
    void on_portfolio_state(const Envelope& env);   // held symbols need bars
    void tick(SysTime now);

    // Pull and publish bars for the given symbols (incremental after the first pull). Returns bars published.
    std::size_t pull_bars(const std::vector<std::string>& symbols, SysTime now, bool full = false);
    std::size_t pull_quotes(const std::vector<std::string>& symbols, SysTime now);
    std::vector<std::string> universe_symbols() const;
    std::vector<std::string> all_symbols() const;

private:
    AlpacaClient& client_;
    IBus& bus_;
    IngestorConfig cfg_;
    const TradingCalendar& cal_;
    std::map<std::string, Date> last_bar_published_;
    std::map<std::string, Date> quote_symbols_;     // symbol -> entry deadline
    std::set<std::string> held_;
    std::map<int, Date> pulled_;                    // pull slot (minutes) -> session pulled
    SysTime last_quote_{};
};

} // namespace at
