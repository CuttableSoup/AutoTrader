#include "strategy/market_store.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace at {

void MarketStore::add_bar(const std::string& symbol, const Bar& bar) {
    auto& s = series_[symbol];
    auto i = lower_bound_date(s, bar.date);
    if (i < s.size() && s[i].date == bar.date) s[i] = bar;
    else s.insert(s.begin() + static_cast<std::ptrdiff_t>(i), bar);
}

void MarketStore::set_quote(const std::string& symbol, const Quote& q) { quotes_[symbol] = q; }

const BarSeries* MarketStore::bars(const std::string& symbol) const {
    auto it = series_.find(symbol);
    return it == series_.end() ? nullptr : &it->second;
}

std::optional<Quote> MarketStore::quote(const std::string& symbol) const {
    auto it = quotes_.find(symbol);
    if (it == quotes_.end()) return std::nullopt;
    return it->second;
}

std::size_t MarketStore::end_index(const std::string& symbol, Date as_of) const {
    const BarSeries* s = bars(symbol);
    if (!s) return 0;
    auto i = lower_bound_date(*s, as_of);
    if (i < s->size() && (*s)[i].date == as_of) return i + 1;
    return i;
}

std::optional<Bar> MarketStore::bar_on(const std::string& symbol, Date d) const {
    const BarSeries* s = bars(symbol);
    if (!s) return std::nullopt;
    auto i = index_of_date(*s, d);
    if (!i) return std::nullopt;
    return (*s)[*i];
}

std::optional<Cents> MarketStore::close_on(const std::string& symbol, Date d) const {
    auto b = bar_on(symbol, d);
    if (!b) return std::nullopt;
    return b->close;
}

std::optional<Bar> MarketStore::last_bar_on_or_before(const std::string& symbol, Date d) const {
    const BarSeries* s = bars(symbol);
    if (!s) return std::nullopt;
    auto n = end_index(symbol, d);
    if (n == 0) return std::nullopt;
    return (*s)[n - 1];
}

std::optional<Date> MarketStore::latest_date(const std::string& symbol) const {
    const BarSeries* s = bars(symbol);
    if (!s || s->empty()) return std::nullopt;
    return s->back().date;
}

std::vector<std::string> MarketStore::symbols() const {
    std::vector<std::string> out;
    out.reserve(series_.size());
    for (const auto& [k, v] : series_) out.push_back(k);
    return out;
}

std::size_t MarketStore::bar_count() const {
    std::size_t n = 0;
    for (const auto& [k, v] : series_) n += v.size();
    return n;
}

void MarketStore::apply_split(const std::string& symbol, Date ex_date, double ratio) {
    auto it = series_.find(symbol);
    if (it == series_.end() || ratio <= 0) return;
    for (auto& b : it->second) {
        if (b.date >= ex_date) break;
        b.open = mul(b.open, 1.0 / ratio);
        b.high = mul(b.high, 1.0 / ratio);
        b.low = mul(b.low, 1.0 / ratio);
        b.close = mul(b.close, 1.0 / ratio);
        b.volume = static_cast<std::int64_t>(static_cast<double>(b.volume) * ratio);
    }
}

std::size_t MarketStore::load_csv(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("market store: cannot open " + file.string());
    std::string line;
    if (!std::getline(in, line)) return 0; if (!line.empty() && line.back() == '\r') line.pop_back();
    // header
    std::vector<std::string> cols;
    { std::stringstream ss(line); std::string c; while (std::getline(ss, c, ',')) cols.push_back(c); }
    auto col = [&](const char* name) {
        for (std::size_t i = 0; i < cols.size(); ++i) if (cols[i] == name) return static_cast<int>(i);
        throw std::runtime_error(std::string("market store: csv missing column ") + name);
    };
    int c_sym = col("symbol"), c_date = col("date"), c_o = col("open"), c_h = col("high"), c_l = col("low"), c_c = col("close"), c_v = col("volume");
    std::size_t n = 0;
    std::vector<std::string> f;
    while (std::getline(in, line)) { if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        f.clear();
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ',')) f.push_back(c);
        if (f.size() < cols.size()) continue;
        Bar b;
        b.date = parse_date_or_throw(f[static_cast<std::size_t>(c_date)]);
        b.open = parse_cents(f[static_cast<std::size_t>(c_o)]);
        b.high = parse_cents(f[static_cast<std::size_t>(c_h)]);
        b.low = parse_cents(f[static_cast<std::size_t>(c_l)]);
        b.close = parse_cents(f[static_cast<std::size_t>(c_c)]);
        b.volume = std::stoll(f[static_cast<std::size_t>(c_v)]);
        // Append fast path when in order.
        auto& s = series_[f[static_cast<std::size_t>(c_sym)]];
        if (s.empty() || s.back().date < b.date) s.push_back(b);
        else add_bar(f[static_cast<std::size_t>(c_sym)], b);
        ++n;
    }
    return n;
}

Bar bar_from_payload(const nlohmann::json& p) {
    Bar b;
    b.date = parse_date_or_throw(p.at("session_date").get<std::string>());
    b.open = p.at("open_cents").get<Cents>();
    b.high = p.at("high_cents").get<Cents>();
    b.low = p.at("low_cents").get<Cents>();
    b.close = p.at("close_cents").get<Cents>();
    b.volume = p.at("volume").get<std::int64_t>();
    return b;
}

nlohmann::json bar_to_payload(const std::string& symbol, const Bar& b, const std::string& source, const std::string& data_ts_utc) {
    return {
        {"kind", "bar"}, {"symbol", symbol}, {"source", source}, {"data_ts_utc", data_ts_utc},
        {"session_date", iso_date(b.date)}, {"open_cents", b.open}, {"high_cents", b.high}, {"low_cents", b.low},
        {"close_cents", b.close}, {"volume", b.volume}, {"adjusted", true},
    };
}

Quote quote_from_payload(const nlohmann::json& p) {
    Quote q;
    q.bid_cents = p.at("bid_cents").get<Cents>();
    q.ask_cents = p.at("ask_cents").get<Cents>();
    q.bid_size = p.value("bid_size", 0LL);
    q.ask_size = p.value("ask_size", 0LL);
    q.halted = p.value("halted", false);
    if (auto t = parse_iso_utc(p.value("data_ts_utc", ""))) q.ts = *t;
    return q;
}

nlohmann::json quote_to_payload(const std::string& symbol, const Quote& q, const std::string& source) {
    return {
        {"kind", "quote"}, {"symbol", symbol}, {"source", source}, {"data_ts_utc", iso_utc(q.ts)},
        {"bid_cents", q.bid_cents}, {"ask_cents", q.ask_cents}, {"bid_size", q.bid_size}, {"ask_size", q.ask_size},
        {"halted", q.halted},
    };
}

} // namespace at
