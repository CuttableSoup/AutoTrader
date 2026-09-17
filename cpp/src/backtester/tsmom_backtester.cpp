#include "backtester/tsmom_backtester.hpp"

#include "strategy/tsmom_signal.hpp"
#include "strategy/tsmom_sizing.hpp"

#include <map>

namespace at {

TsmomBacktestResult run_tsmom_gross_backtest(const MarketStore& mkt, const UniverseSnapshot& universe, const TsmomParams& params,
                                             Date start, Date end, const TradingCalendar& cal) {
    TsmomBacktestResult res;
    auto sessions = cal.sessions(start, end);

    std::map<std::string, double> w_active;   // weight active DURING the session being processed
    std::map<std::string, double> w_prev;     // weight that was active during the PREVIOUS session
    for (const auto& sym : universe.symbols) { w_active[sym] = 0.0; w_prev[sym] = 0.0; }

    for (std::size_t i = 0; i < sessions.size(); ++i) {
        Date session = sessions[i];
        double book_t = 0.0;

        // Overnight leg: yesterday's active weight (w_prev) on the prior-close -> today's-open gap.
        if (i > 0) {
            for (const auto& sym : universe.symbols) {
                double w = w_prev[sym];
                if (w == 0.0) continue;
                const BarSeries* s = mkt.bars(sym);
                if (!s) continue;
                auto idx = index_of_date(*s, session);
                auto idx_prev = index_of_date(*s, sessions[i - 1]);
                if (!idx || !idx_prev) continue;
                Cents open_t = (*s)[*idx].open;
                Cents close_prev = (*s)[*idx_prev].close;
                if (close_prev <= 0) continue;
                book_t += w * simple_return(close_prev, open_t);
            }
        }

        // Intraday leg: today's active weight (w_active, not yet updated by any formation
        // happening today) on the open -> close move.
        for (const auto& sym : universe.symbols) {
            double w = w_active[sym];
            if (w == 0.0) continue;
            const BarSeries* s = mkt.bars(sym);
            if (!s) continue;
            auto idx = index_of_date(*s, session);
            if (!idx) continue;
            Cents open_t = (*s)[*idx].open;
            Cents close_t = (*s)[*idx].close;
            if (open_t <= 0) continue;
            book_t += w * simple_return(open_t, close_t);
        }

        res.dates.push_back(session);
        res.book_return.push_back(book_t);

        // What was active during `session` becomes "yesterday" for the next iteration's
        // overnight leg -- snapshotted before any formation update below, matching
        // tsmom.py's w_overnight[t] = w_intraday[t-1].
        w_prev = w_active;

        // A formation today recomputes weights that take effect from the *next* session's
        // open onward (docs/prereg/TSMOM-v1.md: "held from open of session f+1").
        if (is_formation_session(session, cal)) {
            TsmomSignalContext ctx{mkt, universe, cal, session};
            std::vector<TsmomInstrumentSignal> signals;
            signals.reserve(universe.symbols.size());
            for (const auto& sym : universe.symbols) signals.push_back(evaluate_tsmom_signal(sym, ctx, params));
            for (const auto& w : compute_tsmom_target_weights(signals, params.sizing.gross_cap_pct)) w_active[w.symbol] = w.target_weight;
        }
    }
    return res;
}

} // namespace at
