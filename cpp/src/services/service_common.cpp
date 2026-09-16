#include "services/service_common.hpp"

#include "common/logging.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>

namespace at {

namespace {
std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }
} // namespace

void install_signal_handlers() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
}

bool running() { return g_running.load(); }

nlohmann::json load_json_file(const std::filesystem::path& p) {
    if (!std::filesystem::exists(p)) return nlohmann::json::object();
    std::ifstream in(p);
    nlohmann::json j;
    in >> j;
    return j;
}

void save_json_atomic(const std::filesystem::path& p, const nlohmann::json& j) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p.string() + ".tmp");
    out << j.dump();
    out.close();
    std::filesystem::rename(p.string() + ".tmp", p);
}

bool DailyTrigger::due(SysTime now, const TradingCalendar& cal) {
    NyLocal ny = to_ny(now);
    if (!cal.is_trading_day(ny.date) || ny.minutes_since_midnight() < minutes_et || last_fired == ny.date) return false;
    last_fired = ny.date;
    return true;
}

ServiceContext bootstrap(int argc, char** argv, const std::string& service_name, bool need_secrets) {
    std::string config_path = "config/paper.json";
    std::string level = "info";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc) config_path = argv[++i];
        else if (!std::strcmp(argv[i], "--log-level") && i + 1 < argc) level = argv[++i];
        else if (!std::strcmp(argv[i], "--no-secrets")) need_secrets = false;
        else if (!std::strcmp(argv[i], "--help")) { std::cout << "usage: " << argv[0] << " --config <file> [--log-level info] [--no-secrets]\n"; std::exit(0); }
    }
    ServiceContext ctx;
    ctx.service = service_name;
    try {
        if (!std::filesystem::exists(config_path) && std::filesystem::exists("config/paper.example.json") && config_path == "config/paper.json") config_path = "config/paper.example.json";
        ctx.cfg = Config::load(config_path);
        init_logging(service_name, ctx.cfg.resolve(ctx.cfg.get<std::string>("log_dir", "var/log")).string(), level);
        ctx.env = ctx.cfg.get<std::string>("env", "paper");
        ctx.state_dir = ctx.cfg.resolve(ctx.cfg.get<std::string>("state_dir", "var/state"));
        std::filesystem::create_directories(ctx.state_dir);
        ctx.strategy = StrategyParams::load(ctx.cfg.resolve(ctx.cfg.get<std::string>("strategy_config", "config/strategy.v1.json")));
        ctx.risk = RiskLimits::load(ctx.cfg.resolve(ctx.cfg.get<std::string>("risk_config", "config/risk.v1.json")));
        ctx.registry = std::make_unique<SchemaRegistry>(ctx.cfg.resolve(ctx.cfg.get<std::string>("schemas_dir", "schemas")));
        if (need_secrets) ctx.secrets = Secrets::load(ctx.cfg);
        ctx.bus = std::make_unique<NatsBus>(ctx.cfg.get<std::string>("nats_url", "nats://127.0.0.1:4222"), service_name);
        ctx.bus->ensure_streams(*ctx.registry);
        ctx.bus->set_validator(ctx.registry.get());
        install_signal_handlers();
        spdlog::info("{} up: env={} strategy={} risk={} shadow_mode={}", service_name, ctx.env, ctx.strategy.strategy_version, ctx.risk.risk_version, ctx.risk.shadow_mode);
    } catch (const std::exception& e) {
        std::cerr << service_name << ": bootstrap failed: " << e.what() << "\n";
        std::exit(1);
    }
    return ctx;
}

} // namespace at
