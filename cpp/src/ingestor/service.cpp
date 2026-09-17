#include "ingestor/service.hpp"

#include "strategy/market_store.hpp"
#include "strategy/tsmom_universe.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>

namespace at {

IngestorService::IngestorService(AlpacaClient& client, IBus& bus, IngestorConfig cfg, const TradingCalendar& cal)
    : client_(client), bus_(bus), cfg_(std::move(cfg)), cal_(cal) {}

void IngestorService::on_candidate(const Envelope& env) {
    const auto& p = env.payload;
    if (auto d = parse_date(p.value("entry_deadline_date", ""))) quote_symbols_[p.value("symbol", "")] = *d;
}

void IngestorService::on_portfolio_state(const Envelope& env) {
    held_.clear();
    for (const auto& pos : env.payload.value("positions", nlohmann::json::array())) held_.insert(pos.value("symbol", ""));
}

std::vector<std::string> IngestorService::universe_symbols() const {
    std::vector<std::string> out;
    if (cfg_.universe_file.empty() || !std::filesystem::exists(cfg_.universe_file)) return out;
    try {
        std::ifstream in(cfg_.universe_file);
        nlohmann::json j;
        in >> j;
        for (const auto& s : j.value("symbols", nlohmann::json::array())) out.push_back(s.get<std::string>());
    } catch (const std::exception& e) {
        spdlog::warn("ingestor: cannot read universe file: {}", e.what());
    }
    return out;
}

std::vector<std::string> IngestorService::all_symbols() const {
    std::set<std::string> s;
    for (const auto& x : universe_symbols()) s.insert(x);
    for (const auto& x : held_) s.insert(x);
    for (const auto& [x, d] : quote_symbols_) s.insert(x);
    s.insert(cfg_.trend_symbol);
    return {s.begin(), s.end()};
}

std::vector<std::string> IngestorService::tsmom_symbols() const {
    std::vector<std::string> out;
    out.reserve(kTsmomUniverse.size());
    for (const auto& entry : kTsmomUniverse) out.push_back(entry.first);
    return out;
}

std::size_t IngestorService::pull_bars(const std::vector<std::string>& symbols, SysTime now, bool full) {
    if (symbols.empty()) return 0;
    Date session = cal_.session_for(now);
    Date start = cal_.add_sessions(session, -cfg_.bar_lookback_sessions);
    std::size_t published = 0;
    std::vector<std::string> need;
    for (const auto& s : symbols) if (full || !last_bar_published_.count(s)) need.push_back(s);
    std::vector<std::string> incremental;
    for (const auto& s : symbols) if (!full && last_bar_published_.count(s)) incremental.push_back(s);
    auto publish = [&](const std::map<std::string, BarSeries>& bars) {
        for (const auto& [sym, series] : bars) {
            for (const auto& b : series) {
                auto it = last_bar_published_.find(sym);
                if (it != last_bar_published_.end() && b.date <= it->second) continue;
                bus_.publish("market.data.bar." + sym, make_envelope("ingestor", bar_to_payload(sym, b, "alpaca", iso_utc(now))));
                last_bar_published_[sym] = b.date;
                ++published;
            }
        }
    };
    try {
        if (!need.empty()) publish(client_.daily_bars(need, start, session));
        if (!incremental.empty()) {
            Date inc_start = cal_.add_sessions(session, -3);
            publish(client_.daily_bars(incremental, inc_start, session));
        }
    } catch (const std::exception& e) {
        spdlog::error("ingestor: bar pull failed: {}", e.what());
    }
    spdlog::info("ingestor: pulled bars for {} symbols, published {} bars", symbols.size(), published);
    return published;
}

std::size_t IngestorService::pull_bars_tr(const std::vector<std::string>& symbols, SysTime now, bool full) {
    if (symbols.empty()) return 0;
    Date session = cal_.session_for(now);
    Date start = cal_.add_sessions(session, -cfg_.bar_lookback_sessions);
    std::size_t published = 0;
    std::vector<std::string> need;
    for (const auto& s : symbols) if (full || !last_tr_bar_published_.count(s)) need.push_back(s);
    std::vector<std::string> incremental;
    for (const auto& s : symbols) if (!full && last_tr_bar_published_.count(s)) incremental.push_back(s);
    auto publish = [&](const std::map<std::string, BarSeries>& bars) {
        for (const auto& [sym, series] : bars) {
            for (const auto& b : series) {
                auto it = last_tr_bar_published_.find(sym);
                if (it != last_tr_bar_published_.end() && b.date <= it->second) continue;
                bus_.publish("market.data.bar_tr." + sym, make_envelope("ingestor", bar_to_payload(sym, b, "alpaca", iso_utc(now))));
                last_tr_bar_published_[sym] = b.date;
                ++published;
            }
        }
    };
    try {
        if (!need.empty()) publish(client_.daily_bars(need, start, session, "all"));
        if (!incremental.empty()) {
            Date inc_start = cal_.add_sessions(session, -3);
            publish(client_.daily_bars(incremental, inc_start, session, "all"));
        }
    } catch (const std::exception& e) {
        spdlog::error("ingestor: TR bar pull failed: {}", e.what());
    }
    spdlog::info("ingestor: pulled TR bars for {} symbols, published {} bars", symbols.size(), published);
    return published;
}

std::size_t IngestorService::pull_shortable(const std::vector<std::string>& symbols, SysTime now) {
    if (symbols.empty()) return 0;
    std::size_t n = 0;
    try {
        for (const auto& [sym, ok] : client_.shortable_flags(symbols)) {
            bus_.publish("market.data.shortable." + sym, make_envelope("ingestor", shortable_to_payload(sym, ok, "alpaca", iso_utc(now))));
            ++n;
        }
    } catch (const std::exception& e) {
        spdlog::error("ingestor: shortable pull failed: {}", e.what());
    }
    spdlog::info("ingestor: refreshed shortable flags for {} of {} symbols", n, symbols.size());
    return n;
}

std::size_t IngestorService::pull_quotes(const std::vector<std::string>& symbols, SysTime now) {
    if (symbols.empty()) return 0;
    std::size_t n = 0;
    try {
        for (const auto& [sym, q] : client_.latest_quotes(symbols)) {
            if (q.ask_cents <= 0 || q.bid_cents <= 0) continue;
            bus_.publish("market.data.quote." + sym, make_envelope("ingestor", quote_to_payload(sym, q, "alpaca")));
            ++n;
        }
    } catch (const std::exception& e) {
        spdlog::error("ingestor: quote pull failed: {}", e.what());
    }
    (void)now;
    return n;
}

void IngestorService::tick(SysTime now) {
    NyLocal ny = to_ny(now);
    if (!cal_.is_trading_day(ny.date)) return;
    int m = ny.minutes_since_midnight();
    for (int slot : cfg_.bar_pull_minutes_et) {
        if (m >= slot && pulled_[slot] != ny.date) {
            pulled_[slot] = ny.date;
            pull_bars(all_symbols(), now, false);
            if (cfg_.pull_tsmom_tr_bars) {
                pull_bars_tr(tsmom_symbols(), now, false);
                if (last_shortable_pulled_ != ny.date) { last_shortable_pulled_ = ny.date; pull_shortable(tsmom_symbols(), now); }
            }
        }
    }
    // Quotes for symbols with a live entry deadline, during the open window.
    if (m >= cfg_.quote_window_start_minutes_et && m < cfg_.quote_window_end_minutes_et && now - last_quote_ >= std::chrono::seconds(cfg_.quote_interval_s)) {
        last_quote_ = now;
        std::vector<std::string> syms;
        for (auto it = quote_symbols_.begin(); it != quote_symbols_.end();) {
            if (it->second < ny.date) it = quote_symbols_.erase(it);
            else { syms.push_back(it->first); ++it; }
        }
        if (!syms.empty()) pull_quotes(syms, now);
    }
}

} // namespace at
