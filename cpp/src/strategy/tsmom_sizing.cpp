#include "strategy/tsmom_sizing.hpp"

#include <algorithm>
#include <cmath>

namespace at {

std::vector<TsmomTargetWeight> compute_tsmom_target_weights(const std::vector<TsmomInstrumentSignal>& signals, double gross_cap_pct) {
    double gross = 0.0;
    for (const auto& s : signals) gross += std::fabs(s.raw_weight);
    double cap = gross_cap_pct / 100.0;
    double scale = gross > 0.0 ? std::min(1.0, cap / gross) : 0.0;

    std::vector<TsmomTargetWeight> out;
    out.reserve(signals.size());
    for (const auto& s : signals) {
        TsmomTargetWeight w;
        w.symbol = s.symbol;
        w.asset_class = s.asset_class;
        w.target_weight = s.raw_weight * scale;
        out.push_back(std::move(w));
    }
    return out;
}

TsmomPositionSizingResult size_tsmom_target(const TsmomPositionSizingInput& in) {
    TsmomPositionSizingResult r;
    if (in.equity_cents <= 0 || in.ref_px_cents <= 0) return r;
    double notional = static_cast<double>(in.equity_cents) * in.target_weight;
    double qty = notional / static_cast<double>(in.ref_px_cents);
    r.target_qty = static_cast<std::int64_t>(std::llround(qty));
    r.target_notional_cents = static_cast<Cents>(r.target_qty) * in.ref_px_cents;
    r.target_weight_pct = pct_of(r.target_notional_cents, in.equity_cents);
    return r;
}

} // namespace at
