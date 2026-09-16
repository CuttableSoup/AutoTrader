// Universe rules (docs/DESIGN.md 2.1), applied point-in-time.
#pragma once
#include "strategy/market_store.hpp"
#include "strategy/params.hpp"
#include "strategy/types.hpp"

#include <string>
#include <vector>

namespace at {

// Returns "" if the security passes, otherwise the first failed rule.
std::string universe_exclusion_reason(const SecurityInfo& info, const MarketStore& mkt, Date as_of, const UniverseParams& p);

UniverseSnapshot build_universe(const std::vector<SecurityInfo>& securities, const MarketStore& mkt, Date as_of, const UniverseParams& p);

// Load securities from CSV: symbol,name,sector,category,market_cap,first_listed,analyst_coverage,transcript_available,median_spread_bps,as_of
// market_cap in dollars (decimal). Multiple rows per symbol with different as_of are allowed; the loader keeps all.
std::vector<SecurityInfo> load_securities_csv(const std::filesystem::path& file);

// Point-in-time view: for each symbol, the latest row with as_of <= date.
std::vector<SecurityInfo> securities_as_of(const std::vector<SecurityInfo>& all, Date as_of);

} // namespace at
