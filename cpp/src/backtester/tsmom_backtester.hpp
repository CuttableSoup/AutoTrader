// TSMOM gross daily book-return backtest: a direct, monthly-formation replay over
// MarketStore's total-return-adjusted bars, used only to check the C++ port's
// return-attribution math against python/autotrader/research/tsmom.py::
// portfolio_returns (cpp/tests/test_tsmom_backtest.cpp). Deliberately NOT wired
// through Backtester::run's daily-event-driven earnings loop, nor through
// Ledger/RiskManager: those are earnings/ATR/entry-exit-shaped machinery this
// doesn't need, and this harness's whole job is checking the signal + return
// formula, not re-exercising risk-gate logic (that's tested separately, live-shaped,
// in cpp/tests/test_tsmom_risk.cpp). Per docs/DECISIONS.md's Phase 2 decision 3, this
// does not run DSR/folds/walk-forward -- the Python research already did that; this
// is a parity check that the C++ formula reproduces it.
#pragma once
#include "strategy/market_store.hpp"
#include "strategy/tsmom_params.hpp"
#include "strategy/types.hpp"

#include <vector>

namespace at {

struct TsmomBacktestResult {
    std::vector<Date> dates;
    // Gross daily book return (fraction), split-attributed across the intraday leg
    // (today's active weights, open->close) and the overnight leg (yesterday's active
    // weights, prior close->today's open) -- the split that makes a formation-transition
    // day correct instead of misattributing it whole to one side. Idle capital earns 0
    // (the risk-free credit is a backtest-cost-model nicety, out of scope here; see the
    // fixture export script's docstring). No commission/spread cost is charged.
    std::vector<double> book_return;
};

// [start, end], both inclusive trading sessions. mkt must already hold total-return-
// adjusted open/close bars (see ingestor's market.data.bar_tr.* convention: Bar.close =
// closeadj-equivalent, Bar.open = the matching TR-adjusted open) for every symbol in
// universe, with enough history before `start` for the first in-window formation's
// momentum_lookback_sessions + vol_lookback_sessions warmup.
TsmomBacktestResult run_tsmom_gross_backtest(const MarketStore& mkt, const UniverseSnapshot& universe, const TsmomParams& params,
                                             Date start, Date end, const TradingCalendar& cal);

} // namespace at
