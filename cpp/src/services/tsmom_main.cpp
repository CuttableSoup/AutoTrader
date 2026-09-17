// at_tsmom_svc: market.data (18-ETF TSMOM universe) -> signals.candidate on each monthly
// formation session (~16:25 ET). Separate process from at_strategy_svc: independent
// failure/restart domain, no events.earnings dependency, a genuinely different cadence
// (monthly vs. daily-close). The universe is the fixed, literature-cited 18-ETF list
// (strategy/tsmom_universe.cpp's kTsmomUniverse) -- built in-process via
// build_tsmom_universe(), not loaded from a state file: unlike the earnings universe,
// it is never rebuilt from external data, so there is nothing to persist or reload.
#include "services/service_common.hpp"
#include "strategy/engine.hpp"
#include "strategy/market_store.hpp"
#include "strategy/tsmom_universe.hpp"

#include <spdlog/spdlog.h>

using namespace at;

int main(int argc, char** argv) {
    ServiceContext ctx = bootstrap(argc, argv, "tsmom", false);
    if (!ctx.tsmom) {
        spdlog::error("tsmom: config must set tsmom_config (see config/strategy.v3.json); refusing to start with no TSMOM parameters");
        return 1;
    }
    const TradingCalendar& cal = nyse();
    StrategyEngine engine(StrategyParams{}, cal);
    engine.set_tsmom_params(*ctx.tsmom);
    engine.set_universe(build_tsmom_universe(cal.session_for(now_utc())));
    spdlog::info("tsmom: universe {} with {} members", engine.universe().id, engine.universe().symbols.size());

    // No events.earnings subscription -- TSMOM has no earnings dependency. Subscribes to the
    // dividend-adjusted market.data.bar_tr.* (not market.data.bar.*): SPY is also the
    // earnings strategy's trend_symbol, which needs raw/split-adjusted prices, so the two
    // strategies cannot share one bar subject for it -- see ingestor/service.hpp's
    // pull_bars_tr.
    ctx.bus->subscribe("market.data.bar_tr.*", "tsmom-bars", [&](const Delivery& d) { engine.on_bar(d.env.payload.value("symbol", ""), bar_from_payload(d.env.payload)); });

    std::filesystem::path state_file = ctx.state_file("tsmom_state.json");
    MonthlyTrigger monthly_eval{16 * 60 + 25};   // 5 min after the earnings strategy's 16:20 close_eval
    nlohmann::json st = load_json_file(state_file);
    if (auto d = parse_date(st.value("last_evaluated", ""))) monthly_eval.last_fired = *d;

    run_loop(*ctx.bus, [&](SysTime now) {
        if (!monthly_eval.due(now, cal)) return;
        Date session = cal.session_for(now);
        auto res = engine.evaluate_rebalance_session(session, iso_utc(now));
        spdlog::info("tsmom: formation {} evaluated {} signals -> {} candidates", iso_date(session), res.signals.size(), res.candidates.size());
        for (const auto& c : res.candidates) {
            ctx.bus->publish("signals.candidate", make_envelope("tsmom", c.to_json()));
            spdlog::info("  candidate {} asset_class={} mom_sign={} vol_annual_pct={:.2f} target_weight_pct={:+.2f}", c.symbol, c.asset_class,
                         c.mom_sign.value_or(0), c.vol_annual_pct.value_or(0.0), c.target_weight_pct.value_or(0.0));
        }
        save_json_atomic(state_file, {{"last_evaluated", iso_date(session)}, {"universe_id", engine.universe().id}});
    });
    spdlog::info("tsmom: shutting down");
    return 0;
}
