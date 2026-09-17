// at_risk_svc: signals.candidate + signals.validated + portfolio.state + broker.reconcile + control.* + quotes
//              -> orders.approved (entries at 09:31 ET, exits/stop ratchets at 16:10 ET) and control.* on breaches.
#include "risk/risk_manager.hpp"
#include "services/service_common.hpp"
#include "strategy/market_store.hpp"

#include <spdlog/spdlog.h>

using namespace at;

int main(int argc, char** argv) {
    ServiceContext ctx = bootstrap(argc, argv, "risk", false);
    const TradingCalendar& cal = nyse();
    RiskManager risk(ctx.strategy, ctx.risk, cal, ctx.tsmom);
    std::filesystem::path state_file = ctx.state_file("risk_state.json");
    risk.load_state(load_json_file(state_file));
    auto persist = [&]() { save_json_atomic(state_file, risk.state_json()); };
    auto flush_control = [&]() {
        for (auto& [subj, p] : risk.take_control_messages()) ctx.bus->publish(subj, make_envelope("risk", p));
    };

    ctx.bus->subscribe("signals.candidate", "risk-candidates", [&](const Delivery& d) { risk.on_candidate(d.env); persist(); });
    ctx.bus->subscribe("signals.validated", "risk-validated", [&](const Delivery& d) { risk.on_validated(d.env); flush_control(); persist(); });
    ctx.bus->subscribe("portfolio.state", "risk-portfolio", [&](const Delivery& d) { risk.on_portfolio_state(d.env.payload); flush_control(); });
    ctx.bus->subscribe("broker.reconcile", "risk-reconcile", [&](const Delivery& d) { risk.on_reconcile(d.env.payload); flush_control(); persist(); });
    ctx.bus->subscribe("control.>", "risk-control", [&](const Delivery& d) { if (d.env.producer != "risk") risk.on_control(d.subject, d.env.payload); persist(); });
    ctx.bus->subscribe("market.data.quote.*", "risk-quotes", [&](const Delivery& d) { risk.on_quote(d.env.payload.value("symbol", ""), quote_from_payload(d.env.payload)); });
    ctx.bus->subscribe("market.data.shortable.*", "risk-shortable", [&](const Delivery& d) { risk.on_shortable(d.env.payload.value("symbol", ""), d.env.payload.value("shortable", false)); });
    ctx.bus->subscribe("orders.status", "risk-status", [&](const Delivery& d) { risk.on_order_status(d.env.payload); flush_control(); });
    ctx.bus->subscribe("orders.filled", "risk-filled", [&](const Delivery& d) { risk.on_order_filled(d.env.payload); });

    DailyTrigger open_entries{9 * 60 + 31};
    DailyTrigger retry_entries{9 * 60 + 36};
    DailyTrigger close_sweep{16 * 60 + 10};

    run_loop(*ctx.bus, [&](SysTime now) {
        Date session = cal.session_for(now);
        risk.start_session(session);
        bool do_entries = open_entries.due(now, cal) || (retry_entries.due(now, cal) && risk.pending_count() > 0);
        if (do_entries) {
            auto decisions = risk.process_pending_entries(session, now);
            for (const auto& d : decisions) {
                if (d.approved) {
                    ctx.bus->publish("orders.approved", make_envelope("risk", d.order, d.candidate_msg_id));
                    std::string stop_str = d.order["stop_px_cents"].is_null() ? "-" : cents_to_decimal(d.order["stop_px_cents"].get<Cents>());
                    spdlog::info("risk: APPROVED {} {} qty={} limit={} stop={} binding={}", d.order.value("intent", "ENTRY"), d.symbol, d.order["qty"].get<int>(), cents_to_decimal(d.order["limit_px_cents"].get<Cents>()), stop_str, d.sizing.binding);
                } else {
                    spdlog::info("risk: REJECTED {} reason={}", d.symbol, d.reject_reason);
                }
            }
            flush_control();
            persist();
        }
        if (close_sweep.due(now, cal)) {
            auto orders = risk.end_of_session_sweep(session);
            for (const auto& o : orders) {
                ctx.bus->publish("orders.approved", make_envelope("risk", o));
                spdlog::info("risk: {} {} qty={} reason={}", o["intent"].get<std::string>(), o["symbol"].get<std::string>(), o["qty"].get<int>(), o["reason"].get<std::string>());
            }
            flush_control();
            persist();
        }
    });
    persist();
    spdlog::info("risk: shutting down (paused_new={}, halted={})", risk.paused_new(), risk.halted());
    return 0;
}
