// In-memory daily bar + quote store. Same container feeds the strategy
// engine live (from market.data) and the backtester (from replay).
#pragma once
#include "strategy/types.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace at {

class MarketStore {
public:
    // Upsert by (symbol, date); keeps series sorted.
    void add_bar(const std::string& symbol, const Bar& bar);
    void set_quote(const std::string& symbol, const Quote& q);

    const BarSeries* bars(const std::string& symbol) const;
    std::optional<Quote> quote(const std::string& symbol) const;

    // Number of bars with date <= as_of (exclusive end index for indicators).
    std::size_t end_index(const std::string& symbol, Date as_of) const;
    std::optional<Bar> bar_on(const std::string& symbol, Date d) const;
    std::optional<Cents> close_on(const std::string& symbol, Date d) const;
    // Last bar on or before the date.
    std::optional<Bar> last_bar_on_or_before(const std::string& symbol, Date d) const;
    std::optional<Date> latest_date(const std::string& symbol) const;

    std::vector<std::string> symbols() const;
    std::size_t bar_count() const;
    bool has(const std::string& symbol) const { return series_.count(symbol) > 0; }

    // Apply a split: multiply prices by 1/ratio and volume by ratio for bars strictly before ex_date.
    void apply_split(const std::string& symbol, Date ex_date, double ratio);

    // Load bars from a CSV with header: symbol,date,open,high,low,close,volume (prices as decimals).
    // Returns rows loaded. Throws on malformed input.
    std::size_t load_csv(const std::filesystem::path& file);

private:
    std::map<std::string, BarSeries> series_;
    std::map<std::string, Quote> quotes_;
};

// Parse a market.data payload (bar kind) into a Bar. Throws on missing fields.
Bar bar_from_payload(const nlohmann::json& payload);
nlohmann::json bar_to_payload(const std::string& symbol, const Bar& b, const std::string& source, const std::string& data_ts_utc);
Quote quote_from_payload(const nlohmann::json& payload);
nlohmann::json quote_to_payload(const std::string& symbol, const Quote& q, const std::string& source);
// TSMOM shortable pre-flight (RiskManager::evaluate_rebalance).
nlohmann::json shortable_to_payload(const std::string& symbol, bool shortable, const std::string& source, const std::string& data_ts_utc);

} // namespace at
