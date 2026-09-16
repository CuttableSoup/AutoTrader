// at_ingestor_svc: Alpaca daily bars + quotes -> market.data.bar.* / market.data.quote.*
#include "alpaca/client.hpp"
#include "ingestor/service.hpp"
#include "services/service_common.hpp"

#include <spdlog/spdlog.h>

using namespace at;

namespace {
int minutes_from_hhmm(const std::string& s) {
    if (s.size() < 5) return 0;
    return std::stoi(s.substr(0, 2)) * 60 + std::stoi(s.substr(3, 2));
}
} // namespace

int main(int argc, char** argv) {
    ServiceContext ctx = bootstrap(argc, argv, "ingestor", true);
    AlpacaClient client(ctx.cfg.get<std::string>("alpaca.trading_base_url", "https://paper-api.alpaca.markets"), ctx.cfg.get<std::string>("alpaca.data_base_url", "https://data.alpaca.markets"),
                        ctx.secrets.require("alpaca_key_id"), ctx.secrets.require("alpaca_secret_key"), ctx.cfg.get<int>("alpaca.http_timeout_s", 10), ctx.cfg.get<int>("alpaca.rate_limit_per_min", 200), ctx.cfg.get<std::string>("alpaca.data_feed", "iex"));
    IngestorConfig ic;
    ic.bar_pull_minutes_et.clear();
    for (const auto& t : ctx.cfg.get<std::vector<std::string>>("ingestor.bar_pull_times_et", {"09:20", "16:30"})) ic.bar_pull_minutes_et.push_back(minutes_from_hhmm(t));
    ic.bar_lookback_sessions = ctx.cfg.get<int>("ingestor.bar_lookback_sessions", 300);
    ic.trend_symbol = ctx.strategy.signal.trend_symbol;
    ic.universe_file = ctx.state_file("universe.json");
    IngestorService svc(client, *ctx.bus, ic, nyse());

    ctx.bus->subscribe("signals.candidate", "ingestor-candidates", [&](const Delivery& d) { svc.on_candidate(d.env); });
    ctx.bus->subscribe("portfolio.state", "ingestor-portfolio", [&](const Delivery& d) { svc.on_portfolio_state(d.env); });

    // Initial history pull so the strategy engine has its lookback on day one.
    svc.pull_bars(svc.all_symbols(), now_utc(), true);
    run_loop(*ctx.bus, [&](SysTime now) { svc.tick(now); }, 500);
    spdlog::info("ingestor: shutting down");
    return 0;
}
