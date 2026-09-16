#include "strategy/universe.hpp"

#include "common/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace at {

std::string universe_exclusion_reason(const SecurityInfo& info, const MarketStore& mkt, Date as_of, const UniverseParams& p) {
    if (p.exclude_over_optioned.count(info.symbol)) return "over_optioned";
    if (p.exclude_etfs && info.is_etf()) return "etf";
    if (p.exclude_adrs && info.is_adr()) return "adr";
    if (!info.is_common_stock()) return "not_common_stock";
    if (info.market_cap_cents <= p.min_market_cap_cents) return "market_cap";
    if (info.first_listed && days_between(*info.first_listed, as_of) < p.min_listing_age_days) return "ipo_age";
    if (!info.first_listed) return "listing_date_unknown";
    if (info.analyst_coverage < p.min_analyst_coverage) return "analyst_coverage";
    if (p.require_transcript && !info.transcript_available) return "no_transcript";
    if (info.median_spread_bps >= p.max_median_spread_bps) return "spread";
    const BarSeries* bars = mkt.bars(info.symbol);
    if (!bars) return "no_price_history";
    std::size_t end = mkt.end_index(info.symbol, as_of);
    auto adv = adv_dollars_cents(*bars, end, 20);
    if (!adv) return "insufficient_price_history";
    if (*adv <= p.min_adv20_dollars_cents) return "adv";
    return "";
}

UniverseSnapshot build_universe(const std::vector<SecurityInfo>& securities, const MarketStore& mkt, Date as_of, const UniverseParams& p) {
    UniverseSnapshot u;
    u.as_of = as_of;
    for (const auto& s : securities) {
        std::string why = universe_exclusion_reason(s, mkt, as_of, p);
        if (why.empty()) {
            u.symbols.push_back(s.symbol);
            u.info[s.symbol] = s;
        } else {
            u.exclusion_reasons[s.symbol] = why;
        }
    }
    std::sort(u.symbols.begin(), u.symbols.end());
    std::string key = iso_date(as_of);
    for (const auto& s : u.symbols) { key += '|'; key += s; }
    u.id = sha256_hex(key).substr(0, 16);
    return u;
}

std::vector<SecurityInfo> load_securities_csv(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("securities: cannot open " + file.string());
    std::string line;
    std::vector<SecurityInfo> out;
    if (!std::getline(in, line)) return out; if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> cols;
    { std::stringstream ss(line); std::string c; while (std::getline(ss, c, ',')) cols.push_back(c); }
    std::map<std::string, int> idx;
    for (std::size_t i = 0; i < cols.size(); ++i) idx[cols[i]] = static_cast<int>(i);
    auto need = [&](const char* n) { if (!idx.count(n)) throw std::runtime_error(std::string("securities: missing column ") + n); return idx[n]; };
    int c_sym = need("symbol"), c_name = need("name"), c_sector = need("sector"), c_cat = need("category"), c_cap = need("market_cap"),
        c_first = need("first_listed"), c_cov = need("analyst_coverage"), c_tr = need("transcript_available"), c_spr = need("median_spread_bps"),
        c_asof = need("as_of");
    std::vector<std::string> f;
    while (std::getline(in, line)) { if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        f.clear();
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ',')) f.push_back(c);
        while (f.size() < cols.size()) f.push_back("");
        SecurityInfo s;
        s.symbol = f[static_cast<std::size_t>(c_sym)];
        s.name = f[static_cast<std::size_t>(c_name)];
        s.sector = f[static_cast<std::size_t>(c_sector)];
        s.category = f[static_cast<std::size_t>(c_cat)];
        s.market_cap_cents = f[static_cast<std::size_t>(c_cap)].empty() ? 0 : parse_cents(f[static_cast<std::size_t>(c_cap)]);
        s.first_listed = parse_date(f[static_cast<std::size_t>(c_first)]);
        s.analyst_coverage = f[static_cast<std::size_t>(c_cov)].empty() ? 0 : std::stoi(f[static_cast<std::size_t>(c_cov)]);
        const auto& tr = f[static_cast<std::size_t>(c_tr)];
        s.transcript_available = tr == "1" || tr == "true" || tr == "True";
        s.median_spread_bps = f[static_cast<std::size_t>(c_spr)].empty() ? 0.0 : std::stod(f[static_cast<std::size_t>(c_spr)]);
        s.as_of = parse_date_or_throw(f[static_cast<std::size_t>(c_asof)]);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<SecurityInfo> securities_as_of(const std::vector<SecurityInfo>& all, Date as_of) {
    std::map<std::string, const SecurityInfo*> best;
    for (const auto& s : all) {
        if (s.as_of > as_of) continue;
        auto it = best.find(s.symbol);
        if (it == best.end() || it->second->as_of < s.as_of) best[s.symbol] = &s;
    }
    std::vector<SecurityInfo> out;
    out.reserve(best.size());
    for (const auto& [k, v] : best) out.push_back(*v);
    return out;
}

} // namespace at
