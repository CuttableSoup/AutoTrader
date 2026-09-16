// Reconciler: broker truth vs the internal ledger, at startup and every 15
// minutes. CLEAN unblocks trading; MISMATCH pauses new entries and pages
// (control.pause_new on the bus + POST /alert to the watchdog).
#pragma once
#include "alpaca/client.hpp"
#include "alpaca/http.hpp"
#include "common/bus.hpp"

#include <memory>
#include <set>
#include <string>

namespace at {

struct ReconcilerConfig {
    int interval_s = 900;
    std::string watchdog_url;
    std::string watchdog_token;
    double avg_px_tolerance_pct = 1.0;
    int startup_wait_s = 20;          // wait this long for the first portfolio.state before a STARTUP reconcile
};

class ReconcilerService {
public:
    ReconcilerService(AlpacaClient& client, IBus& bus, ReconcilerConfig cfg);

    void on_portfolio_state(const Envelope& env);
    void on_submitted(const Envelope& env);   // learn our broker order ids (orphan detection)
    void tick(SysTime now);
    // Runs one reconcile and publishes broker.reconcile. Returns the payload.
    nlohmann::json run(const std::string& trigger);
    bool have_internal_view() const { return internal_.is_object(); }

private:
    void page(const std::string& reason, const nlohmann::json& diffs);

    AlpacaClient& client_;
    IBus& bus_;
    ReconcilerConfig cfg_;
    nlohmann::json internal_;          // latest portfolio.state payload
    std::string internal_as_of_;
    std::set<std::string> our_broker_ids_;
    SysTime last_run_{};
    SysTime started_{};
    bool startup_done_ = false;
    std::unique_ptr<HttpClient> watchdog_http_;
};

// Pure comparison used by the service and the tests. our_ids: broker order ids this system created.
nlohmann::json reconcile_diff(const std::vector<AlpacaPosition>& broker, const std::vector<AlpacaOrder>& open_orders, const nlohmann::json& internal_state, double avg_px_tol_pct);

} // namespace at
