#include "strategy/tsmom_universe.hpp"

#include "common/sha256.hpp"

#include <algorithm>

namespace at {

const std::vector<std::pair<std::string, std::string>> kTsmomUniverse = {
    {"SPY", "EQUITY"}, {"QQQ", "EQUITY"}, {"IWM", "EQUITY"}, {"EFA", "EQUITY"}, {"EEM", "EQUITY"},
    {"TLT", "RATES_CREDIT"}, {"IEF", "RATES_CREDIT"}, {"SHY", "RATES_CREDIT"}, {"LQD", "RATES_CREDIT"}, {"HYG", "RATES_CREDIT"},
    {"GLD", "COMMODITIES"}, {"SLV", "COMMODITIES"}, {"USO", "COMMODITIES"}, {"DBC", "COMMODITIES"},
    {"UUP", "CURRENCIES"}, {"FXE", "CURRENCIES"}, {"FXY", "CURRENCIES"}, {"FXB", "CURRENCIES"},
};

UniverseSnapshot build_tsmom_universe(Date as_of) {
    UniverseSnapshot u;
    u.as_of = as_of;
    for (const auto& [sym, asset_class] : kTsmomUniverse) {
        u.symbols.push_back(sym);
        SecurityInfo info;
        info.symbol = sym;
        info.category = "ETF";
        info.asset_class = asset_class;
        info.as_of = as_of;
        u.info[sym] = info;
    }
    std::sort(u.symbols.begin(), u.symbols.end());
    std::string key = iso_date(as_of);
    for (const auto& s : u.symbols) { key += '|'; key += s; }
    u.id = sha256_hex(key).substr(0, 16);
    return u;
}

} // namespace at
