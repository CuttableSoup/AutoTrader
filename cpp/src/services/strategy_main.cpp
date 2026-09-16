// at_strategy_svc: market.data + events.earnings (+ universe snapshot file) -> signals.candidate at the close.
#include "services/service_common.hpp"
#include "strategy/engine.hpp"
#include "strategy/market_store.hpp"

#include <spdlog/spdlog.h>

using namespace at;

int main(int argc, char** argv) {
    ServiceContext ctx = bootstrap(argc, argv, "strategy", false);
    const TradingCalendar& cal = nyse();
    StrategyEngine engine(ctx.strategy, cal);
    std::filesystem::path universe_file = ctx.state_file("universe.json");
    std::filesystem::path state_file = ctx.state_file("strategy_state.json");
    std::string loaded_universe_id;

    auto load_universe = [&]() {
        nlohmann::json u = load_json_file(universe_file);
        if (!u.is_object() || !u.contains("symbols")) return;
        std::string id = u.value("id", "");
        if (id == loaded_universe_id) return;
        UniverseSnapshot snap;
        snap.id = id;
        if (auto d = parse_date(u.value("as_of", ""))) snap.as_of = *d;
        for (const auto& s : u["symbols"]) {
            std::string sym = s.get<std::string>();
            snap.symbols.push_back(sym);
            SecurityInfo info;
            info.symbol = sym;
            info.category = "Domestic Common Stock";
            if (u.contains("sectors") && u["sectors"].contains(sym)) info.sector = u["sectors"][sym].get<std::string>();
            snap.info[sym] = info;
        }
        engine.set_universe(snap);
        loaded_universe_id = id;
        spdlog::info("strategy: universe {} as of {} with {} members", snap.id, iso_date(snap.as_of), snap.symbols.size());
    };
    load_universe();

    // Replay: durable consumers deliver the full retained history on first start (bars 14 d, events 400 d).
    ctx.bus->subscribe("market.data.bar.*", "strategy-bars", [&](const Delivery& d) { engine.on_bar(d.env.payload.value("symbol", ""), bar_from_payload(d.env.payload)); });
    ctx.bus->subscribe("events.earnings", "strategy-events", [&](const Delivery& d) { engine.on_earnings_event(EarningsEvent::from_json(d.env.payload)); });

    DailyTrigger close_eval{16 * 60 + 20};
    nlohmann::json st = load_json_file(state_file);
    if (auto d = parse_date(st.value("last_evaluated", ""))) close_eval.last_fired = *d;

    run_loop(*ctx.bus, [&](SysTime now) {
        if (!close_eval.due(now, cal)) return;
        load_universe();
        Date session = cal.session_for(now);
        auto res = engine.evaluate_session(session, iso_utc(now));
        spdlog::info("strategy: session {} evaluated {} events -> {} candidates (spy_above_trend={})", iso_date(session), res.evaluations.size(), res.candidates.size(), res.spy_above_trend ? (*res.spy_above_trend ? "yes" : "no") : "unknown");
        for (const auto& e : res.evaluations) if (!e.passed) spdlog::debug("  {} failed: {}", e.candidate ? e.candidate->symbol : "?", nlohmann::json(e.failed).dump());
        for (const auto& c : res.candidates) {
            ctx.bus->publish("signals.candidate", make_envelope("strategy", c.to_json()));
            spdlog::info("  candidate {} ear={:.2f} vol={:.2f} mom={:.0f}", c.symbol, c.ear_pct, c.vol_ratio, c.mom_pct);
        }
        save_json_atomic(state_file, {{"last_evaluated", iso_date(session)}, {"universe_id", loaded_universe_id}});
    });
    spdlog::info("strategy: shutting down");
    return 0;
}
