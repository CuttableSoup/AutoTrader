// at_portfolio_svc: broker + fills + bars + events -> portfolio.state (60 s + every fill) and the watchdog heartbeat (10 s).
#include "alpaca/client.hpp"
#include "portfolio/service.hpp"
#include "services/service_common.hpp"

#include <spdlog/spdlog.h>

using namespace at;

int main(int argc, char** argv) {
    ServiceContext ctx = bootstrap(argc, argv, "portfolio", true);
    AlpacaClient client(ctx.cfg.get<std::string>("alpaca.trading_base_url", "https://paper-api.alpaca.markets"), ctx.cfg.get<std::string>("alpaca.data_base_url", "https://data.alpaca.markets"),
                        ctx.secrets.require("alpaca_key_id"), ctx.secrets.require("alpaca_secret_key"), ctx.cfg.get<int>("alpaca.http_timeout_s", 10), ctx.cfg.get<int>("alpaca.rate_limit_per_min", 200), ctx.cfg.get<std::string>("alpaca.data_feed", "iex"));
    PortfolioConfig pc;
    pc.state_file = ctx.state_file("portfolio_state.json");
    pc.watchdog_url = ctx.cfg.get<std::string>("portfolio.watchdog_url", "");
    pc.watchdog_token = ctx.secrets.get("watchdog_token", "");
    pc.heartbeat_interval_s = ctx.cfg.get<int>("portfolio.heartbeat_interval_s", 10);
    pc.publish_interval_s = ctx.cfg.get<int>("portfolio.publish_interval_s", 60);
    PortfolioService svc(client, *ctx.bus, ctx.strategy, pc, nyse());
    svc.startup();

    ctx.bus->subscribe("orders.approved", "portfolio-approved", [&](const Delivery& d) { svc.on_approved(d.env); });
    ctx.bus->subscribe("orders.submitted", "portfolio-submitted", [&](const Delivery& d) { svc.on_submitted(d.env); });
    ctx.bus->subscribe("orders.status", "portfolio-status", [&](const Delivery& d) { svc.on_status(d.env); });
    ctx.bus->subscribe("orders.filled", "portfolio-filled", [&](const Delivery& d) { svc.on_filled(d.env); });
    ctx.bus->subscribe("market.data.bar.*", "portfolio-bars", [&](const Delivery& d) { svc.on_bar(d.env); });
    ctx.bus->subscribe("market.data.quote.*", "portfolio-quotes", [&](const Delivery& d) { svc.on_quote(d.env); });
    ctx.bus->subscribe("events.earnings", "portfolio-events", [&](const Delivery& d) { svc.on_earnings(d.env); });
    ctx.bus->subscribe("broker.reconcile", "portfolio-reconcile", [&](const Delivery& d) { svc.on_reconcile(d.env); });
    ctx.bus->subscribe("control.>", "portfolio-control", [&](const Delivery& d) { svc.on_control(d.subject, d.env.payload); });
    ctx.bus->subscribe("signals.validated", "portfolio-validated", [&](const Delivery& d) { svc.on_validated(d.env); });

    run_loop(*ctx.bus, [&](SysTime now) { svc.tick(now); }, 250);
    svc.save_state();
    spdlog::info("portfolio: shutting down");
    return 0;
}
