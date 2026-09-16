// at_reconciler_svc: broker vs internal ledger at startup and every 15 min -> broker.reconcile (+ pause/page on mismatch).
#include "alpaca/client.hpp"
#include "reconciler/service.hpp"
#include "services/service_common.hpp"

#include <spdlog/spdlog.h>

using namespace at;

int main(int argc, char** argv) {
    ServiceContext ctx = bootstrap(argc, argv, "reconciler", true);
    AlpacaClient client(ctx.cfg.get<std::string>("alpaca.trading_base_url", "https://paper-api.alpaca.markets"), ctx.cfg.get<std::string>("alpaca.data_base_url", "https://data.alpaca.markets"),
                        ctx.secrets.require("alpaca_key_id"), ctx.secrets.require("alpaca_secret_key"), ctx.cfg.get<int>("alpaca.http_timeout_s", 10), ctx.cfg.get<int>("alpaca.rate_limit_per_min", 200), ctx.cfg.get<std::string>("alpaca.data_feed", "iex"));
    ReconcilerConfig rc;
    rc.interval_s = ctx.cfg.get<int>("reconciler.interval_s", 900);
    rc.watchdog_url = ctx.cfg.get<std::string>("portfolio.watchdog_url", "");
    rc.watchdog_token = ctx.secrets.get("watchdog_token", "");
    rc.startup_wait_s = ctx.cfg.get<int>("reconciler.startup_wait_s", 20);
    ReconcilerService svc(client, *ctx.bus, rc);

    ctx.bus->subscribe("portfolio.state", "reconciler-portfolio", [&](const Delivery& d) { svc.on_portfolio_state(d.env); });
    ctx.bus->subscribe("orders.submitted", "reconciler-submitted", [&](const Delivery& d) { svc.on_submitted(d.env); });
    ctx.bus->subscribe("control.>", "reconciler-control", [&](const Delivery& d) {
        // Operator can force a run: control.resume with details.reconcile=true
        if (d.subject == "control.resume" && d.env.payload.value("details", nlohmann::json::object()).value("reconcile", false)) svc.run("MANUAL");
    });

    run_loop(*ctx.bus, [&](SysTime now) { svc.tick(now); }, 500);
    spdlog::info("reconciler: shutting down");
    return 0;
}
