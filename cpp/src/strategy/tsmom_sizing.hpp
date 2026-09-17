// TSMOM-v1 portfolio construction and position sizing (docs/prereg/TSMOM-v1.md).
// Deliberately separate from sizing.cpp's size_position(): that function is
// unsigned/long-only, requires atr20_cents (which TSMOM has no analog of), and
// applies a per-name cap stack (single-name/sector/gap budget) with no TSMOM
// equivalent. TSMOM's own signal already vol-targets per instrument via
// raw_weight = mom_sign/vol_annual, so size_position's book-level vol-target
// band-scaling is deliberately NOT reused here either -- stacking a second,
// independent vol-target mechanism on top would double up two undefined-
// combination scalars.
#pragma once
#include "common/money.hpp"
#include "strategy/tsmom_signal.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace at {

struct TsmomTargetWeight {
    std::string symbol;
    std::string asset_class;
    double target_weight = 0;   // signed, gross-normalized w_i
};

// Cross-sectional normalization, a direct port of tsmom.py::compute_weights' portfolio
// step: gross = sum(|raw_w|); scale = min(1, gross_cap/gross) if gross > 0 else 0;
// w = raw_w * scale. DOWN-scale only, never levers above gross_cap_pct/100.
std::vector<TsmomTargetWeight> compute_tsmom_target_weights(const std::vector<TsmomInstrumentSignal>& signals, double gross_cap_pct = 100.0);

struct TsmomPositionSizingInput {
    Cents equity_cents = 0;
    Cents ref_px_cents = 0;   // expected fill (limit) price
    double target_weight = 0; // signed
};

struct TsmomPositionSizingResult {
    std::int64_t target_qty = 0;        // signed: negative means short
    Cents target_notional_cents = 0;    // signed
    double target_weight_pct = 0;       // signed, notional/equity
};

// Converts a signed target weight into a signed target share qty, given equity and a
// reference price. Pure and stateless like size_position() -- the caller (RiskManager)
// diffs this absolute target against the current position to get the order delta.
TsmomPositionSizingResult size_tsmom_target(const TsmomPositionSizingInput& in);

} // namespace at
