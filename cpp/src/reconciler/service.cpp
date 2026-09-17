#include "reconciler/service.hpp"

#include "common/ids.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <map>

namespace at {

nlohmann::json reconcile_diff(const std::vector<AlpacaPosition>& broker, const std::vector<AlpacaOrder>& open_orders, const nlohmann::json& internal, double tol_pct) {
    nlohmann::json diffs = nlohmann::json::array();
    nlohmann::json bpos = nlohmann::json::array(), ipos = nlohmann::json::array(), orphans = nlohmann::json::array();
    std::map<std::string, const AlpacaPosition*> b;
    for (const auto& p : broker) { b[p.symbol] = &p; bpos.push_back({{"symbol", p.symbol}, {"qty", p.qty}, {"avg_px_cents", p.avg_entry_px_cents}}); }
    std::map<std::string, nlohmann::json> i;
    for (const auto& p : internal.value("positions", nlohmann::json::array())) { i[p.value("symbol", "")] = p; ipos.push_back({{"symbol", p.value("symbol", "")}, {"qty", p.value("qty", 0LL)}, {"avg_px_cents", p.value("avg_px_cents", 0LL)}}); }

    // Resting sell stops per symbol (protection).
    std::map<std::string, std::int64_t> protected_qty;
    for (const auto& o : open_orders) {
        auto consider = [&](const AlpacaOrder& x) {
            if (x.side == "sell" && (x.type == "stop" || x.type == "stop_limit") && (x.status == "new" || x.status == "held" || x.status == "accepted")) protected_qty[x.symbol] += x.qty;
        };
        consider(o);
        for (const auto& l : o.legs) consider(l);
        if (!is_our_client_order_id(o.client_order_id) && !o.parent_id) orphans.push_back(o.client_order_id);
    }

    for (const auto& [sym, bp] : b) {
        auto it = i.find(sym);
        if (it == i.end()) { diffs.push_back({{"symbol", sym}, {"field", "missing_internal"}, {"broker", bp->qty}, {"internal", nullptr}}); continue; }
        std::int64_t iq = it->second.value("qty", 0LL);
        if (iq != bp->qty) diffs.push_back({{"symbol", sym}, {"field", "qty"}, {"broker", bp->qty}, {"internal", iq}});
        Cents ia = it->second.value("avg_px_cents", 0LL);
        if (ia > 0 && bp->avg_entry_px_cents > 0 && std::fabs(pct_of(ia - bp->avg_entry_px_cents, bp->avg_entry_px_cents)) > tol_pct)
            diffs.push_back({{"symbol", sym}, {"field", "avg_px_cents"}, {"broker", bp->avg_entry_px_cents}, {"internal", ia}});
        // TSMOM positions (tagged with asset_class) never carry a resting stop by design --
        // the strategy has no stop-loss mechanism, only the next monthly rebalance. Without
        // this guard every TSMOM long would permanently reconcile as "unprotected" and pause
        // new entries forever.
        bool is_tsmom_position = !it->second.value("asset_class", std::string()).empty();
        if (!is_tsmom_position && bp->qty > 0 && protected_qty[sym] < bp->qty)
            diffs.push_back({{"symbol", sym}, {"field", "unprotected_position"}, {"broker", bp->qty}, {"internal", protected_qty[sym]}});
    }
    for (const auto& [sym, ip] : i)
        if (!b.count(sym)) diffs.push_back({{"symbol", sym}, {"field", "missing_broker"}, {"broker", nullptr}, {"internal", ip.value("qty", 0LL)}});

    std::string status = (diffs.empty() && orphans.empty()) ? "CLEAN" : "MISMATCH";
    return {{"status", status}, {"broker_positions", bpos}, {"internal_positions", ipos}, {"diffs", diffs}, {"open_orders_count", open_orders.size()}, {"orphan_orders", orphans}, {"error", nullptr}};
}

ReconcilerService::ReconcilerService(AlpacaClient& client, IBus& bus, ReconcilerConfig cfg) : client_(client), bus_(bus), cfg_(std::move(cfg)) {
    started_ = now_utc();
    if (!cfg_.watchdog_url.empty())
        watchdog_http_ = std::make_unique<HttpClient>(std::map<std::string, std::string>{{"Authorization", "Bearer " + cfg_.watchdog_token}, {"Content-Type", "application/json"}}, 3, 600);
}

void ReconcilerService::on_portfolio_state(const Envelope& env) {
    if (env.ts_utc >= internal_as_of_) { internal_ = env.payload; internal_as_of_ = env.ts_utc; }
}

void ReconcilerService::on_submitted(const Envelope& env) {
    our_broker_ids_.insert(env.payload.value("broker_order_id", ""));
    for (const auto& l : env.payload.value("legs", nlohmann::json::array())) our_broker_ids_.insert(l.value("broker_order_id", ""));
}

void ReconcilerService::tick(SysTime now) {
    if (!startup_done_) {
        bool waited = now - started_ > std::chrono::seconds(cfg_.startup_wait_s);
        if (have_internal_view() || waited) {
            startup_done_ = true;
            last_run_ = now;
            run("STARTUP");
        }
        return;
    }
    if (now - last_run_ >= std::chrono::seconds(cfg_.interval_s)) {
        last_run_ = now;
        run("TIMER");
    }
}

nlohmann::json ReconcilerService::run(const std::string& trigger) {
    nlohmann::json payload;
    try {
        auto broker = client_.positions();
        auto orders = client_.orders("open", true);
        nlohmann::json internal = have_internal_view() ? internal_ : nlohmann::json{{"positions", nlohmann::json::array()}};
        payload = reconcile_diff(broker, orders, internal, cfg_.avg_px_tolerance_pct);
        if (!have_internal_view() && !broker.empty()) {
            payload["status"] = "MISMATCH";
            payload["error"] = "no internal portfolio state received; broker holds positions";
        }
    } catch (const std::exception& e) {
        payload = {{"status", "ERROR"}, {"broker_positions", nlohmann::json::array()}, {"internal_positions", nlohmann::json::array()}, {"diffs", nlohmann::json::array()}, {"open_orders_count", 0}, {"orphan_orders", nlohmann::json::array()}, {"error", e.what()}};
    }
    payload["trigger"] = trigger;
    payload["checked_at_utc"] = now_utc_iso();
    std::string status = payload["status"].get<std::string>();
    if (status == "CLEAN") spdlog::info("reconcile {}: CLEAN ({} positions, {} open orders)", trigger, payload["broker_positions"].size(), payload["open_orders_count"].get<int>());
    else spdlog::error("reconcile {}: {} diffs={} orphans={} error={}", trigger, status, payload["diffs"].dump(), payload["orphan_orders"].dump(), payload["error"].dump());
    bus_.publish("broker.reconcile", make_envelope("reconciler", payload));
    if (status != "CLEAN") {
        std::string reason = "broker reconcile " + status + (payload["error"].is_string() ? ": " + payload["error"].get<std::string>() : "");
        bus_.publish("control.pause_new", make_envelope("reconciler", {{"command", "pause_new"}, {"reason", reason}, {"source", "reconciler"}, {"issued_at_utc", now_utc_iso()}, {"trigger", "RECONCILE_MISMATCH"}, {"details", {{"diffs", payload["diffs"]}, {"orphan_orders", payload["orphan_orders"]}}}, {"drill", false}}));
        page(reason, payload["diffs"]);
    }
    return payload;
}

void ReconcilerService::page(const std::string& reason, const nlohmann::json& diffs) {
    if (!watchdog_http_) return;
    try {
        nlohmann::json body = {{"trigger", "RECONCILE_MISMATCH"}, {"reason", reason}, {"source", "reconciler"}, {"diffs", diffs}};
        auto r = watchdog_http_->post(cfg_.watchdog_url + "/alert", body.dump());
        spdlog::warn("reconcile: watchdog alerted (HTTP {})", r.status);
    } catch (const std::exception& e) {
        spdlog::error("reconcile: watchdog alert failed: {}", e.what());
    }
}

} // namespace at
