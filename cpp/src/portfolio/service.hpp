// Portfolio service: broker truth + strategy metadata -> portfolio.state
// (every fill + 60 s timer) and the watchdog heartbeat (every 10 s, HTTP + bus).
#pragma once
#include "alpaca/client.hpp"
#include "alpaca/http.hpp"
#include "common/bus.hpp"
#include "common/calendar.hpp"
#include "strategy/ledger.hpp"
#include "strategy/params.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace at {

struct PortfolioConfig {
    std::filesystem::path state_file;
    std::string watchdog_url;        // "" disables the HTTP heartbeat
    std::string watchdog_token;
    int heartbeat_interval_s = 10;
    int publish_interval_s = 60;
    int end_of_session_minutes = 16 * 60 + 5;   // ET
};

class PortfolioService {
public:
    PortfolioService(AlpacaClient& client, IBus& bus, StrategyParams params, PortfolioConfig cfg, const TradingCalendar& cal);

    // Pull account + positions from the broker, merge with persisted metadata. reconciled stays false
    // until broker.reconcile reports CLEAN.
    void startup();

    void on_approved(const Envelope& env);     // cache entry metadata by client_order_id
    void on_submitted(const Envelope& env);    // stop-leg broker ids
    void on_status(const Envelope& env);       // replaced stops
    void on_filled(const Envelope& env);
    void on_bar(const Envelope& env);
    void on_quote(const Envelope& env);
    void on_earnings(const Envelope& env);
    void on_reconcile(const Envelope& env);
    void on_control(const std::string& subject, const nlohmann::json& payload);
    void on_validated(const Envelope& env);    // error streak for the heartbeat

    // Timer hook; call every ~1 s.
    void tick(SysTime now);
    void publish_state(SysTime now);
    void heartbeat(SysTime now);

    const Ledger& ledger() const { return ledger_; }
    bool reconciled() const { return reconciled_; }
    void save_state() const;

private:
    void sync_from_broker(SysTime now);
    void end_of_session(Date session);
    std::optional<bool> spy_above_trend() const;
    std::optional<double> realized_vol() const;

    AlpacaClient& client_;
    IBus& bus_;
    StrategyParams params_;
    PortfolioConfig cfg_;
    const TradingCalendar& cal_;
    Ledger ledger_{0};
    MarketStore mkt_;
    std::map<std::string, nlohmann::json> approved_by_coid_;   // pending entry metadata
    std::map<std::string, std::string> stop_leg_by_symbol_;    // symbol -> broker order id of the resting stop
    std::map<std::string, Cents> stop_px_by_symbol_;
    nlohmann::json open_orders_ = nlohmann::json::array();
    bool reconciled_ = false;
    bool halted_ = false;
    int validator_error_streak_ = 0;
    std::string last_market_data_utc_;
    Date session_{};
    Date last_eos_session_{};
    SysTime last_publish_{};
    SysTime last_heartbeat_{};
    SysTime last_broker_sync_{};
    std::int64_t heartbeat_seq_ = 0;
    int new_positions_today_ = 0;
    int orders_today_ = 0;
    int rejects_today_ = 0;
    std::unique_ptr<HttpClient> watchdog_http_;
    Dedupe seen_{200000};
};

} // namespace at
