// Shared bootstrap for every C++ service: args, logging, config, secrets,
// strategy/risk params, schema registry, NATS bus, signals, state files.
#pragma once
#include "common/calendar.hpp"
#include "common/config.hpp"
#include "common/nats_bus.hpp"
#include "common/schema.hpp"
#include "common/secrets.hpp"
#include "risk/limits.hpp"
#include "strategy/params.hpp"
#include "strategy/tsmom_params.hpp"
#include "strategy/tsmom_signal.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace at {

struct ServiceContext {
    std::string service;
    Config cfg;
    Secrets secrets;
    StrategyParams strategy;
    std::optional<TsmomParams> tsmom;   // populated only when the config names a tsmom_config path
    RiskLimits risk;
    std::unique_ptr<SchemaRegistry> registry;
    std::unique_ptr<NatsBus> bus;
    std::filesystem::path state_dir;
    std::string env;              // paper | live

    std::filesystem::path state_file(const std::string& name) const { return state_dir / name; }
};

// --config <file> --log-level <lvl> --no-secrets. Exits the process on failure.
ServiceContext bootstrap(int argc, char** argv, const std::string& service_name, bool need_secrets = true);

void install_signal_handlers();
bool running();

nlohmann::json load_json_file(const std::filesystem::path& p);           // {} if missing
void save_json_atomic(const std::filesystem::path& p, const nlohmann::json& j);

// Fires once per trading day at or after minutes_et (New York wall clock).
struct DailyTrigger {
    int minutes_et;
    Date last_fired{};
    bool due(SysTime now, const TradingCalendar& cal);
};

// Fires once per calendar month, on the formation session (the last trading session of
// the month, strategy/tsmom_signal.hpp's is_formation_session), at or after minutes_et.
struct MonthlyTrigger {
    int minutes_et;
    Date last_fired{};
    bool due(SysTime now, const TradingCalendar& cal);
};

// Sleep-based main loop helper: polls the bus and calls fn(now) every iteration.
template <typename Fn>
void run_loop(NatsBus& bus, Fn&& fn, int idle_ms = 200) {
    while (running()) {
        std::size_t n = bus.poll();
        fn(now_utc());
        if (n == 0) std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
    }
}

} // namespace at
