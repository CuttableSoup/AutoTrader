// Position sizing (docs/DESIGN.md 2.2): equal-weight target with inverse-vol
// tilt, book vol-targeting, single-name / sector / gross caps, gap budget and
// per-position risk at the stop. Returns whole shares.
#pragma once
#include "common/money.hpp"
#include "strategy/params.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace at {

struct SizingInput {
    Cents equity_cents = 0;
    Cents entry_px_cents = 0;                 // expected fill (limit) price
    Cents atr20_cents = 0;
    std::optional<double> stock_vol_annual;   // fraction (0.30 = 30%)
    std::optional<double> book_vol_annual;    // realised book vol, fraction
    int open_positions = 0;
    Cents gross_exposure_cents = 0;
    Cents sector_exposure_cents = 0;          // current exposure in the candidate's sector
    double per_position_risk_pct = 0.5;       // risk.v1.json
    double gross_cap_pct = 100.0;             // risk.v1.json
    bool size_halved = false;                 // drawdown -8% regime
};

struct SizingResult {
    std::int64_t qty = 0;
    Cents notional_cents = 0;
    double weight_pct = 0;                    // notional / equity
    std::string binding;                      // name of the binding constraint
    std::vector<std::pair<std::string, Cents>> caps; // every cap and the notional it allowed
    Cents stop_px_cents = 0;
};

SizingResult size_position(const SizingInput& in, const SizingParams& p, const ExitParams& e);

} // namespace at
