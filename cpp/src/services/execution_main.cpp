// at_execution_svc: orders.approved -> Alpaca; trade_updates WS -> orders.status / orders.filled.
#include "alpaca/client.hpp"
#include "alpaca/trade_stream.hpp"
#include "execution/engine.hpp"
#include "services/service_common.hpp"

#include <spdlog/spdlog.h>

using namespace at;

int main(int argc, char** argv) {
    ServiceContext ctx = bootstrap(argc, argv, "execution", true);
    AlpacaClient client(ctx.cfg.get<std::string>("alpaca.trading_base_url", "https://paper-api.alpaca.markets"), ctx.cfg.get<std::string>("alpaca.data_base_url", "https://data.alpaca.markets"),
                        ctx.secrets.require("alpaca_key_id"), ctx.secrets.require("alpaca_secret_key"), ctx.cfg.get<int>("alpaca.http_timeout_s", 10), ctx.cfg.get<int>("alpaca.rate_limit_per_min", 200), ctx.cfg.get<std::string>("alpaca.data_feed", "iex"));
    std::vector<int> backoff = ctx.cfg.get<std::vector<int>>("execution.retry_backoff_ms", {500, 1500, 4000});
    ExecutionEngine engine(client, *ctx.bus, ctx.state_file("execution_state.json"), ctx.cfg.get<int>("execution.max_submit_attempts", 3), backoff);
    if (engine.halted()) spdlog::critical("execution: starting HALTED (persisted); no submissions until the state file is cleared by an operator");

    TradeUpdatesStream stream(ctx.cfg.get<std::string>("alpaca.trade_updates_ws_url", "wss://paper-api.alpaca.markets/stream"), ctx.secrets.require("alpaca_key_id"), ctx.secrets.require("alpaca_secret_key"));
    stream.start();

    ctx.bus->subscribe("orders.approved", "execution-approved", [&](const Delivery& d) { engine.on_approved(d.env); });
    ctx.bus->subscribe("control.>", "execution-control", [&](const Delivery& d) { engine.on_control(d.subject, d.env.payload); });

    SysTime last_ws_warn{};
    run_loop(*ctx.bus, [&](SysTime now) {
        for (const auto& upd : stream.drain()) {
            try { engine.on_trade_update(upd); }
            catch (const std::exception& e) { spdlog::error("execution: trade update failed: {} ({})", e.what(), upd.dump().substr(0, 200)); }
        }
        if (!stream.listening() && now - last_ws_warn > std::chrono::seconds(30)) {
            last_ws_warn = now;
            spdlog::warn("execution: trade_updates stream not listening (connected={}, reconnects={})", stream.connected(), stream.reconnects());
        }
    }, 100);
    stream.stop();
    engine.save_state();
    spdlog::info("execution: shutting down");
    return 0;
}
