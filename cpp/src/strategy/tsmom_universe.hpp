// The fixed 18-ETF TSMOM-v1 universe (docs/prereg/TSMOM-v1.md). No rebuild, no
// exclusion rules: a pre-vetted, literature-cited fixed list, not a point-in-time
// filter like universe.cpp's build_universe (which is earnings-strategy-shaped
// and explicitly excludes ETFs).
#pragma once
#include "strategy/types.hpp"

#include <string>
#include <utility>
#include <vector>

namespace at {

// symbol -> asset_class ("EQUITY", "RATES_CREDIT", "COMMODITIES", "CURRENCIES").
extern const std::vector<std::pair<std::string, std::string>> kTsmomUniverse;

// Builds a static UniverseSnapshot from kTsmomUniverse. Same sha256(as_of|symbols)[0:16]
// id scheme as build_universe, for consistency with tooling that keys on universe_id.
UniverseSnapshot build_tsmom_universe(Date as_of);

} // namespace at
