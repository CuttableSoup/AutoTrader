#include "portfolio/service.hpp"

#include "strategy/exits.hpp"
#include "strategy/signal.hpp"

#include "common/json_util.hpp"
#include <spdlog/spdlog.h>

#include <fstream>

namespace at {

PortfolioService::PortfolioService(AlpacaClient& client, IBus& bus, StrategyParams params, PortfolioConfig cfg, const TradingCalendar& cal)
    : client_(client), bus_(bus), params_(std::move(params)), cfg_(std::move(cfg)), cal_(cal) {
    if (!cfg_.watchdog_url.empty())
        watchdog_http_ = std::make_unique<HttpClient>(std::map<std::string, std::string>{{"Authorization", "Bearer " + cfg_.watchdog_token}, {"Content-Type", "application/json"}}, 3, 600);
}

void PortfolioService::startup() {
    if (!cfg_.state_file.empty() && std::filesystem::exists(cfg_.state_file)) {
        std::ifstream in(cfg_.state_file);
        nlohmann::json j;
        in >> j;
        ledger_.load_state(j.value("ledger", nlohmann::json::object()));
        for (auto& [k, v] : json_obj(j, "approved_by_coid").items()) approved_by_coid_[k] = v;
        for (auto& [k, v] : json_obj(j, "stop_leg_by_symbol").items()) stop_leg_by_symbol_[k] = v.get<std::string>();
        for (auto& [k, v] : json_obj(j, "stop_px_by_symbol").items()) stop_px_by_symbol_[k] = v.get<Cents>();
        spdlog::info("portfolio: loaded state ({} positions, {} trades)", ledger_.positions().size(), ledger_.trades().size());
    }
    SysTime now = now_utc();
    session_ = cal_.session_for(now);
    sync_from_broker(now);
    reconciled_ = false;
    publish_state(now);
}

void PortfolioService::sync_from_broker(SysTime now) {
    try {
        AlpacaAccount acct = client_.account();
        auto pos = client_.positions();
        std::vector<PositionState> bp;
        for (const auto& p : pos) {
            PositionState x;
            x.symbol = p.symbol; x.qty = p.qty; x.avg_px_cents = p.avg_entry_px_cents; x.last_px_cents = p.current_px_cents;
            bp.push_back(x);
        }
        ledger_.sync_from_broker(bp, session_, iso_utc(now));
        ledger_.set_cash(acct.cash_cents);
        open_orders_ = nlohmann::json::array();
        for (const auto& o : client_.orders("open", true)) {
            open_orders_.push_back({{"client_order_id", o.client_order_id}, {"broker_order_id", o.id}, {"symbol", o.symbol}, {"side", o.side}, {"qty", o.qty}, {"status", o.status}, {"order_type", o.type},
                                    {"stop_px_cents", o.stop_px_cents ? nlohmann::json(*o.stop_px_cents) : nlohmann::json(nullptr)}, {"limit_px_cents", o.limit_px_cents ? nlohmann::json(*o.limit_px_cents) : nlohmann::json(nullptr)}});
            auto record_stop = [&](const AlpacaOrder& x) {
                if (x.side == "sell" && (x.type == "stop" || x.type == "stop_limit") && x.stop_px_cents) {
                    stop_leg_by_symbol_[x.symbol] = x.id;
                    stop_px_by_symbol_[x.symbol] = *x.stop_px_cents;
                    ledger_.set_stop(x.symbol, *x.stop_px_cents, x.id);
                }
            };
            record_stop(o);
            for (const auto& l : o.legs) if (l.status == "new" || l.status == "held" || l.status == "accepted") record_stop(l);
        }
        last_broker_sync_ = now;
        spdlog::info("portfolio: broker sync equity={} cash={} positions={} open_orders={}", format_money(acct.equity_cents), format_money(acct.cash_cents), pos.size(), open_orders_.size());
    } catch (const std::exception& e) {
        spdlog::error("portfolio: broker sync failed: {}", e.what());
    }
}

void PortfolioService::on_approved(const Envelope& env) {
    if (!seen_.first_time(env.msg_id)) return;
    const auto& p = env.payload;
    if (p.value("intent", "") == "ENTRY") approved_by_coid_[p.value("client_order_id", "")] = p;
    ++orders_today_;
    save_state();
}

void PortfolioService::on_submitted(const Envelope& env) {
    if (!seen_.first_time(env.msg_id)) return;
    const auto& p = env.payload;
    std::string sym = p.value("symbol", "");
    for (const auto& leg : p.value("legs", nlohmann::json::array()))
        if (leg.value("leg_type", "") == "stop_loss") { stop_leg_by_symbol_[sym] = leg.value("broker_order_id", ""); if (leg["px_cents"].is_number()) stop_px_by_symbol_[sym] = leg["px_cents"].get<Cents>(); }
    if (p.value("order_type", "") == "stop") { stop_leg_by_symbol_[sym] = p.value("broker_order_id", ""); if (p["stop_px_cents"].is_number()) stop_px_by_symbol_[sym] = p["stop_px_cents"].get<Cents>(); }
    if (auto pos = ledger_.position(sym); pos && stop_px_by_symbol_.count(sym)) ledger_.set_stop(sym, stop_px_by_symbol_[sym], stop_leg_by_symbol_[sym]);
    save_state();
}

void PortfolioService::on_status(const Envelope& env) {
    if (!seen_.first_time(env.msg_id)) return;
    const auto& p = env.payload;
    if (p.value("event", "") == "rejected") ++rejects_today_;
}

void PortfolioService::on_filled(const Envelope& env) {
    if (!seen_.first_time(env.msg_id)) return;
    const auto& p = env.payload;
    std::string sym = p.value("symbol", "");
    std::string intent = p.value("intent", "UNKNOWN");
    std::int64_t qty = p.value("qty", 0LL);
    Cents px = p.value("avg_px_cents", 0LL);
    Cents costs = p.value("commission_cents", 0LL) + p.value("fees_cents", 0LL);
    SysTime now = now_utc();
    Date session = cal_.session_for(now);
    if (intent == "ENTRY" || (p.value("side", "") == "buy" && intent == "UNKNOWN")) {
        // Broker truth: any buy fill is a position, even if the intent map was lost. The reconciler flags it if unprotected.
        if (intent == "UNKNOWN") spdlog::warn("portfolio: buy fill {} x{} with unknown intent; adopting as ENTRY", sym, qty);
        EntryMeta m;
        auto it = approved_by_coid_.find(p.value("client_order_id", ""));
        if (it != approved_by_coid_.end()) {
            const auto& a = it->second;
            m.atr20_cents = a.value("atr20_cents", 0LL);
            m.stop_px_cents = a.value("stop_px_cents", 0LL);
            if (a.contains("candidate_msg_id") && a["candidate_msg_id"].is_string()) m.candidate_msg_id = a["candidate_msg_id"].get<std::string>();
            approved_by_coid_.erase(it);
        }
        m.next_report_date = std::nullopt;
        m.exit_deadline = compute_exit_deadline(session, std::nullopt, params_.exit, cal_);
        if (stop_leg_by_symbol_.count(sym)) m.stop_broker_order_id = stop_leg_by_symbol_[sym];
        ledger_.apply_entry_fill(sym, qty, px, costs, session, p.value("filled_ts_utc", iso_utc(now)), m);
        ++new_positions_today_;
        spdlog::info("portfolio: ENTRY fill {} x{} @ {}", sym, qty, cents_to_decimal(px));
    } else if (p.value("side", "") == "sell") {
        ledger_.apply_exit_fill(sym, qty, px, costs, session, intent);
        if (intent == "EXIT_TREND_SCALE") ledger_.mark_scaled_down(sym);
        if (!ledger_.position(sym)) { stop_leg_by_symbol_.erase(sym); stop_px_by_symbol_.erase(sym); }
        spdlog::info("portfolio: {} fill {} x{} @ {}", intent, sym, qty, cents_to_decimal(px));
    }
    save_state();
    sync_from_broker(now);   // broker is the source of truth for cash/qty after every fill
    publish_state(now);
}

void PortfolioService::on_bar(const Envelope& env) {
    const auto& p = env.payload;
    if (p.value("kind", "") != "bar") return;
    std::string sym = p.value("symbol", "");
    Bar b = bar_from_payload(p);
    mkt_.add_bar(sym, b);
    ledger_.mark(sym, b.close);
    last_market_data_utc_ = p.value("data_ts_utc", env.ts_utc);
}

void PortfolioService::on_quote(const Envelope& env) {
    const auto& p = env.payload;
    if (p.value("kind", "") != "quote") return;
    Quote q = quote_from_payload(p);
    if (q.mid() > 0) ledger_.mark(p.value("symbol", ""), q.mid());
    last_market_data_utc_ = p.value("data_ts_utc", env.ts_utc);
}

void PortfolioService::on_earnings(const Envelope& env) {
    const auto& p = env.payload;
    std::string sym = p.value("symbol", "");
    auto d = parse_date(p.value("report_date", ""));
    if (!d || !ledger_.position(sym)) return;
    if (*d > session_) {
        ledger_.update_next_report_date(sym, *d, cal_, params_.exit.drift_window_sessions, params_.exit.exit_sessions_before_earnings);
        save_state();
    }
}

void PortfolioService::on_reconcile(const Envelope& env) {
    std::string status = env.payload.value("status", "ERROR");
    reconciled_ = status == "CLEAN";
    spdlog::info("portfolio: reconcile {} -> reconciled={}", status, reconciled_);
    publish_state(now_utc());
}

void PortfolioService::on_control(const std::string& subject, const nlohmann::json& p) {
    if (subject.ends_with(".halt") || p.value("command", "") == "halt") halted_ = true;
}

void PortfolioService::on_validated(const Envelope& env) {
    if (env.payload.value("verdict", "") == "ERROR") ++validator_error_streak_; else validator_error_streak_ = 0;
}

std::optional<bool> PortfolioService::spy_above_trend() const { return trend_filter(mkt_, session_, params_.signal); }

void PortfolioService::end_of_session(Date session) {
    ledger_.mark_all_from(mkt_, session);
    ledger_.end_of_session(session);
    last_eos_session_ = session;
    new_positions_today_ = 0;
    orders_today_ = 0;
    rejects_today_ = 0;
    save_state();
    spdlog::info("portfolio: end of session {} equity={} positions={}", iso_date(session), format_money(ledger_.equity()), ledger_.positions().size());
}

void PortfolioService::tick(SysTime now) {
    Date s = cal_.session_for(now);
    if (s != session_) { session_ = s; ledger_.start_session(s); }
    NyLocal ny = to_ny(now);
    if (cal_.is_trading_day(ny.date) && ny.minutes_since_midnight() >= cfg_.end_of_session_minutes && last_eos_session_ != ny.date) end_of_session(ny.date);
    if (now - last_broker_sync_ > std::chrono::seconds(cfg_.publish_interval_s)) sync_from_broker(now);
    if (now - last_publish_ >= std::chrono::seconds(cfg_.publish_interval_s)) publish_state(now);
    if (now - last_heartbeat_ >= std::chrono::seconds(cfg_.heartbeat_interval_s)) heartbeat(now);
}

void PortfolioService::publish_state(SysTime now) {
    last_publish_ = now;
    nlohmann::json p = ledger_.portfolio_state_payload(iso_utc(now), session_, reconciled_, open_orders_, new_positions_today_, orders_today_, rejects_today_, spy_above_trend());
    try { bus_.publish("portfolio.state", make_envelope("portfolio", p)); }
    catch (const std::exception& e) { spdlog::error("portfolio: publish failed: {}", e.what()); }
}

void PortfolioService::heartbeat(SysTime now) {
    last_heartbeat_ = now;
    nlohmann::json hb = {
        {"seq", ++heartbeat_seq_}, {"service", "portfolio"}, {"healthy", !halted_}, {"reconciled", reconciled_}, {"equity_cents", ledger_.equity()},
        {"drawdown_pct", ledger_.drawdown_pct()}, {"last_market_data_utc", last_market_data_utc_.empty() ? nlohmann::json(nullptr) : nlohmann::json(last_market_data_utc_)},
        {"validator_error_streak", validator_error_streak_}, {"sent_at_utc", iso_utc(now)},
    };
    if (watchdog_http_) {
        try {
            auto r = watchdog_http_->post(cfg_.watchdog_url + "/heartbeat", hb.dump());
            if (!r.ok()) spdlog::warn("portfolio: watchdog heartbeat HTTP {}", r.status);
        } catch (const std::exception& e) { spdlog::warn("portfolio: watchdog heartbeat failed: {}", e.what()); }
    }
    try { bus_.publish("watchdog.heartbeat", make_envelope("portfolio", hb)); }
    catch (const std::exception& e) { spdlog::warn("portfolio: heartbeat publish failed: {}", e.what()); }
}

void PortfolioService::save_state() const {
    if (cfg_.state_file.empty()) return;
    nlohmann::json j = {{"ledger", ledger_.state_json()}, {"approved_by_coid", approved_by_coid_}, {"stop_leg_by_symbol", stop_leg_by_symbol_}, {"stop_px_by_symbol", stop_px_by_symbol_}, {"saved_at_utc", now_utc_iso()}};
    std::filesystem::create_directories(cfg_.state_file.parent_path());
    std::ofstream out(cfg_.state_file.string() + ".tmp");
    out << j.dump();
    out.close();
    std::filesystem::rename(cfg_.state_file.string() + ".tmp", cfg_.state_file);
}

} // namespace at
