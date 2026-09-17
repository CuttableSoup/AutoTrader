// TSMOM-v1 per-instrument signal (docs/prereg/TSMOM-v1.md): 12-month sign-of-return,
// inverse-vol scaled. A faithful, line-for-line port of
// python/autotrader/research/tsmom.py::compute_weights' per-instrument step. Pure
// functions over MarketStore so replay and live agree, mirroring signal.cpp's shape
// for the earnings strategy but deliberately not sharing code with it -- the two
// signals have nothing in common beyond "reads bars, returns a number."
#pragma once
#include "strategy/market_store.hpp"
#include "strategy/tsmom_params.hpp"
#include "strategy/types.hpp"

#include <string>

namespace at {

struct TsmomSignalContext {
    const MarketStore& mkt;
    const UniverseSnapshot& universe;   // the static 18-ETF snapshot
    const TradingCalendar& cal;
    Date formation_session{};           // f: the month-end session just closed
};

struct TsmomInstrumentSignal {
    std::string symbol;
    std::string asset_class;
    bool ok = false;           // false if insufficient history (< momentum_lookback+1 or < vol_lookback+1 sessions)
    double mom_sign = 0;       // sign(close[f]/close[f-lookback]-1) in {-1,0,1}
    double vol_annual = 0;     // annualized stdev of daily total returns, trailing vol_lookback sessions (fraction)
    double raw_weight = 0;     // mom_sign / vol_annual, 0 if vol_annual <= 0
};

// Per-instrument raw signal only (no cross-sectional normalization -- that's
// tsmom_sizing::compute_tsmom_target_weights, which needs all instruments at once).
TsmomInstrumentSignal evaluate_tsmom_signal(const std::string& symbol, const TsmomSignalContext& ctx, const TsmomParams& params);

// True iff `session` is the last trading session of its calendar month (matches
// python/autotrader/research/etf_panel.py::month_end_indices() exactly).
bool is_formation_session(Date session, const TradingCalendar& cal);

} // namespace at
