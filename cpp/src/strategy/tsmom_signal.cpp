#include "strategy/tsmom_signal.hpp"

#include <cmath>

namespace at {

bool is_formation_session(Date session, const TradingCalendar& cal) {
    Date next = cal.next_trading_day(session);
    return year_of(next) != year_of(session) || month_of(next) != month_of(session);
}

TsmomInstrumentSignal evaluate_tsmom_signal(const std::string& symbol, const TsmomSignalContext& ctx, const TsmomParams& params) {
    TsmomInstrumentSignal out;
    out.symbol = symbol;
    if (auto it = ctx.universe.info.find(symbol); it != ctx.universe.info.end()) out.asset_class = it->second.asset_class;

    const BarSeries* s = ctx.mkt.bars(symbol);
    if (!s) return out;
    std::size_t end = ctx.mkt.end_index(symbol, ctx.formation_session);
    if (end == 0 || (*s)[end - 1].date != ctx.formation_session) return out;   // no bar exactly on the formation session

    // mom = sign(c[f]/c[f-lookback] - 1), f = end-1. momentum_skip(s, end, lookback, 0)
    // computes exactly (c[end-1]/c[end-1-lookback] - 1), i.e. c[f]/c[f-lookback] - 1.
    auto mom_ret = momentum_skip(*s, end, params.signal.momentum_lookback_sessions, 0);
    if (!mom_ret) return out;   // insufficient history for the lookback window
    double sign = (*mom_ret > 0.0) - (*mom_ret < 0.0);

    // vol = stdev(daily total returns over [f-vol_lookback+1, f], ddof=1) * sqrt(252).
    auto vol_lookback = static_cast<std::size_t>(params.signal.vol_lookback_sessions);
    if (vol_lookback < 2 || end < vol_lookback + 1) return out;
    std::vector<double> rets;
    rets.reserve(vol_lookback);
    for (std::size_t i = end - vol_lookback; i < end; ++i) {
        if ((*s)[i - 1].close <= 0 || (*s)[i].close <= 0) return out;
        rets.push_back(simple_return((*s)[i - 1].close, (*s)[i].close));
    }
    double vol_annual = stdev(rets) * std::sqrt(252.0);

    out.ok = true;
    out.mom_sign = sign;
    out.vol_annual = vol_annual;
    out.raw_weight = vol_annual > 0.0 ? sign / vol_annual : 0.0;
    return out;
}

} // namespace at
