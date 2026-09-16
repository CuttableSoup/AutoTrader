#include "strategy/sizing.hpp"

#include "strategy/exits.hpp"

#include <algorithm>
#include <cmath>

namespace at {

SizingResult size_position(const SizingInput& in, const SizingParams& p, const ExitParams& e) {
    SizingResult r;
    if (in.equity_cents <= 0 || in.entry_px_cents <= 0 || in.atr20_cents <= 0 || p.max_positions <= 0) {
        r.binding = "invalid_input";
        return r;
    }
    const double equity = static_cast<double>(in.equity_cents);
    const double n = static_cast<double>(p.max_positions);

    // Target weight: equal weight, tilted by inverse vol so each name contributes ~target/sqrt(N).
    double w = 1.0 / n;
    if (p.inverse_vol_tilt && in.stock_vol_annual && *in.stock_vol_annual > 0.01) {
        double per_name_vol = (p.target_vol_annual_pct / 100.0) / std::sqrt(n);
        w = per_name_vol / *in.stock_vol_annual;
    }
    // Book-level vol targeting: scale toward target when realised book vol is outside the band.
    if (in.book_vol_annual && *in.book_vol_annual > 0.01) {
        double bv = *in.book_vol_annual * 100.0;
        if (bv > p.target_vol_band_hi_pct || bv < p.target_vol_band_lo_pct) {
            double scalar = std::clamp(p.target_vol_annual_pct / bv, 0.5, 1.25);
            w *= scalar;
        }
    }
    if (in.size_halved) w *= 0.5;

    double target = equity * w;
    r.caps.emplace_back("target", static_cast<Cents>(target));

    auto cap = [&](const char* name, double allowed) {
        r.caps.emplace_back(name, static_cast<Cents>(std::max(0.0, allowed)));
        if (allowed < target) { target = allowed; r.binding = name; }
    };

    const double px = static_cast<double>(in.entry_px_cents);
    const double atr = static_cast<double>(in.atr20_cents);
    r.stop_px_cents = initial_stop_px(in.entry_px_cents, in.atr20_cents, e);
    const double risk_per_share = px - static_cast<double>(r.stop_px_cents);

    cap("single_name_cap", equity * p.single_name_cap_pct / 100.0);
    if (risk_per_share > 0) cap("per_position_risk", equity * in.per_position_risk_pct / 100.0 * px / risk_per_share);
    cap("gap_budget", equity * p.gap_max_equity_pct / 100.0 * px / (p.gap_atr_mult * atr));
    cap("gross_exposure", equity * in.gross_cap_pct / 100.0 - static_cast<double>(in.gross_exposure_cents));
    cap("sector_cap", equity * p.sector_cap_pct / 100.0 - static_cast<double>(in.sector_exposure_cents));
    if (in.open_positions >= p.max_positions) cap("max_positions", 0.0);

    if (r.binding.empty()) r.binding = "target";
    r.qty = target > 0 ? static_cast<std::int64_t>(std::floor(target / px)) : 0;
    if (r.qty < 0) r.qty = 0;
    r.notional_cents = r.qty * in.entry_px_cents;
    r.weight_pct = pct_of(r.notional_cents, in.equity_cents);
    return r;
}

} // namespace at
