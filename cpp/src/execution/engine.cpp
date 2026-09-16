#include "execution/engine.hpp"

#include "common/ids.hpp"

#include "common/json_util.hpp"
#include <spdlog/spdlog.h>

#include <fstream>

namespace at {

nlohmann::json OrderMeta::to_json() const {
    return {{"intent", intent}, {"symbol", symbol}, {"candidate_msg_id", candidate_msg_id}, {"approved_msg_id", approved_msg_id}, {"broker_order_id", broker_order_id}};
}

OrderMeta OrderMeta::from_json(const nlohmann::json& j) {
    OrderMeta m;
    m.intent = j.value("intent", "UNKNOWN");
    m.symbol = j.value("symbol", "");
    m.candidate_msg_id = j.value("candidate_msg_id", "");
    m.approved_msg_id = j.value("approved_msg_id", "");
    m.broker_order_id = j.value("broker_order_id", "");
    return m;
}

ExecutionEngine::ExecutionEngine(AlpacaClient& client, IBus& bus, std::filesystem::path state_file, int max_attempts, std::vector<int> backoff_ms)
    : client_(client), bus_(bus), state_file_(std::move(state_file)), max_attempts_(max_attempts), backoff_ms_(std::move(backoff_ms)) {
    if (!state_file_.empty() && std::filesystem::exists(state_file_)) {
        std::ifstream in(state_file_);
        nlohmann::json j;
        in >> j;
        load_state(j);
    }
}

void ExecutionEngine::remember(const std::string& coid, const OrderMeta& m) {
    by_coid_[coid] = m;
    if (!m.broker_order_id.empty()) broker_to_coid_[m.broker_order_id] = coid;
    save_state();
}

std::string ExecutionEngine::intent_for(const AlpacaOrder& o) const {
    auto it = by_coid_.find(o.client_order_id);
    if (it != by_coid_.end()) return it->second.intent;
    if (o.parent_id) {
        auto pit = broker_to_coid_.find(*o.parent_id);
        if (pit != broker_to_coid_.end()) return o.type == "limit" ? "TAKE_PROFIT_LEG" : "STOP_LEG";
    }
    return "UNKNOWN";
}

void ExecutionEngine::on_control(const std::string& subject, const nlohmann::json& p) {
    std::string cmd = p.value("command", "");
    if (subject.ends_with(".halt") || cmd == "halt") {
        halted_ = true;
        spdlog::critical("execution: HALT ({}); no further submissions", p.value("reason", ""));
    } else if (subject.ends_with(".flatten") || cmd == "flatten") {
        spdlog::critical("execution: FLATTEN ({}): cancel all + close all", p.value("reason", ""));
        try { client_.cancel_all(); } catch (const std::exception& e) { spdlog::error("flatten cancel_all failed: {}", e.what()); }
        try { client_.close_all(true); } catch (const std::exception& e) { spdlog::error("flatten close_all failed: {}", e.what()); }
    } else if (subject.ends_with(".resume") || cmd == "resume") {
        // A halt is cleared only by an explicit operator resume carrying details.clear_halt=true (post-mortem done).
        if (halted_ && p.value("source", "") == "operator" && p.value("details", nlohmann::json::object()).value("clear_halt", false)) {
            halted_ = false;
            spdlog::warn("execution: halt cleared by operator ({})", p.value("reason", ""));
        }
    }
    save_state();
}

void ExecutionEngine::on_approved(const Envelope& env) {
    if (!seen_.first_time(env.msg_id)) return;
    const auto& p = env.payload;
    std::string coid = p.value("client_order_id", "");
    if (by_coid_.count(coid) && !by_coid_[coid].broker_order_id.empty()) {
        spdlog::info("execution: {} already submitted as {} (redelivery)", coid, by_coid_[coid].broker_order_id);
        return;
    }
    if (halted_) { spdlog::warn("execution: halted; dropping {} {}", p.value("intent", ""), coid); return; }
    std::string intent = p.value("intent", "");
    try {
        if (intent == "ENTRY") submit_entry(env, p);
        else if (intent == "STOP_REPLACE") replace_stop(env, p);
        else if (intent == "FLATTEN") on_control("control.flatten", {{"command", "flatten"}, {"reason", p.value("reason", "")}});
        else submit_exit(env, p);
    } catch (const std::exception& e) {
        spdlog::error("execution: {} {} failed: {}", intent, coid, e.what());
        // Surface as a rejected status so the risk manager's reject-rate guard sees it.
        nlohmann::json st = {{"client_order_id", coid}, {"broker_order_id", ""}, {"parent_broker_order_id", nullptr}, {"symbol", p.value("symbol", "")}, {"side", p.value("side", "buy")},
                             {"event", "rejected"}, {"status", "rejected"}, {"qty", p.value("qty", 0)}, {"filled_qty", 0}, {"filled_avg_px_cents", nullptr}, {"price_cents", nullptr},
                             {"position_qty", nullptr}, {"event_ts_utc", now_utc_iso()}, {"raw", {{"error", e.what()}}}};
        bus_.publish("orders.status", make_envelope("execution", st, env.msg_id));
    }
}

void ExecutionEngine::submit_entry(const Envelope& env, const nlohmann::json& p) {
    OrderRequest r;
    r.symbol = p["symbol"].get<std::string>();
    r.qty = p["qty"].get<std::int64_t>();
    r.side = "buy";
    r.type = "limit";
    r.tif = p.value("tif", "gtc");
    r.limit_px_cents = p["limit_px_cents"].get<Cents>();
    r.client_order_id = p["client_order_id"].get<std::string>();
    r.order_class = p.value("order_type", "oto");
    r.stop_loss_px_cents = p["stop_px_cents"].get<Cents>();
    if (p.contains("take_profit_px_cents") && p["take_profit_px_cents"].is_number()) r.take_profit_px_cents = p["take_profit_px_cents"].get<Cents>();
    if (r.order_class == "oto" && r.take_profit_px_cents) r.take_profit_px_cents.reset();
    // Register the intent BEFORE the POST: the trade_updates fill can arrive before (or instead of) the REST response.
    OrderMeta m{"ENTRY", r.symbol, p.value("candidate_msg_id", ""), env.msg_id, ""};
    remember(r.client_order_id, m);
    SubmitResult res = client_.submit(r, max_attempts_, backoff_ms_);
    m.broker_order_id = res.order.id;
    remember(r.client_order_id, m);
    for (const auto& leg : res.order.legs) remember(leg.client_order_id, OrderMeta{leg.type == "limit" ? "TAKE_PROFIT_LEG" : "STOP_LEG", r.symbol, m.candidate_msg_id, env.msg_id, leg.id});
    publish_submitted(env, p, res, r.order_class);
    spdlog::info("execution: ENTRY {} {} x{} limit {} stop {} -> {} (attempts {}, deduped {})", r.symbol, r.client_order_id, r.qty, cents_to_decimal(*r.limit_px_cents), cents_to_decimal(*r.stop_loss_px_cents), res.order.id, res.attempts, res.deduped_at_broker);
}

void ExecutionEngine::submit_exit(const Envelope& env, const nlohmann::json& p) {
    std::string sym = p["symbol"].get<std::string>();
    std::string intent = p.value("intent", "EXIT_TIME");
    std::int64_t qty = p["qty"].get<std::int64_t>();
    // 1. Take the resting stop leg off first so the two sells cannot both fill.
    if (p.contains("linked_broker_order_id") && p["linked_broker_order_id"].is_string()) {
        std::string leg = p["linked_broker_order_id"].get<std::string>();
        try { client_.cancel(leg); spdlog::info("execution: canceled resting stop {} for {}", leg, sym); }
        catch (const std::exception& e) { spdlog::warn("execution: cancel stop {} failed ({}); continuing with exit", leg, e.what()); }
    }
    // 2. Market sell.
    OrderRequest r;
    r.symbol = sym;
    r.qty = qty;
    r.side = "sell";
    r.type = "market";
    r.tif = "day";
    r.client_order_id = p["client_order_id"].get<std::string>();
    remember(r.client_order_id, OrderMeta{intent, sym, "", env.msg_id, ""});
    SubmitResult res = client_.submit(r, max_attempts_, backoff_ms_);
    remember(r.client_order_id, OrderMeta{intent, sym, "", env.msg_id, res.order.id});
    publish_submitted(env, p, res, "market");
    spdlog::info("execution: {} {} x{} -> {}", intent, sym, qty, res.order.id);
    // 3. Partial exit (trend scale-down): re-arm a stop for what remains.
    if (intent == "EXIT_TREND_SCALE" && p.contains("stop_px_cents") && p["stop_px_cents"].is_number()) {
        std::int64_t remaining = 0;
        for (const auto& pos : client_.positions()) if (pos.symbol == sym) remaining = pos.qty - qty;
        if (remaining > 0) {
            OrderRequest s;
            s.symbol = sym; s.qty = remaining; s.side = "sell"; s.type = "stop"; s.tif = "gtc";
            s.stop_px_cents = p["stop_px_cents"].get<Cents>();
            s.client_order_id = client_order_id_for_stop_replace(sym, env.ts_utc.substr(0, 10), *s.stop_px_cents);
            SubmitResult sr = client_.submit(s, max_attempts_, backoff_ms_);
            remember(s.client_order_id, OrderMeta{"STOP_LEG", sym, "", env.msg_id, sr.order.id});
            nlohmann::json p2 = p; p2["client_order_id"] = s.client_order_id; p2["qty"] = remaining;
            publish_submitted(env, p2, sr, "stop");
        }
    }
}

void ExecutionEngine::replace_stop(const Envelope& env, const nlohmann::json& p) {
    std::string sym = p["symbol"].get<std::string>();
    Cents new_stop = p["stop_px_cents"].get<Cents>();
    std::string coid = p["client_order_id"].get<std::string>();
    SubmitResult res;
    if (p.contains("linked_broker_order_id") && p["linked_broker_order_id"].is_string()) {
        std::string leg = p["linked_broker_order_id"].get<std::string>();
        try {
            res.order = client_.replace(leg, std::nullopt, std::nullopt, new_stop, coid);
        } catch (const AlpacaError& e) {
            if (e.status == 422 && e.body.find("client_order_id") != std::string::npos) {
                if (auto ex = client_.order_by_client_id(coid)) { res.order = *ex; res.deduped_at_broker = true; }
                else throw;
            } else throw;
        }
    } else {
        // No resting stop known (e.g. adopted position): place a standalone GTC stop for the full position.
        std::int64_t qty = p["qty"].get<std::int64_t>();
        OrderRequest s;
        s.symbol = sym; s.qty = qty; s.side = "sell"; s.type = "stop"; s.tif = "gtc"; s.stop_px_cents = new_stop; s.client_order_id = coid;
        res = client_.submit(s, max_attempts_, backoff_ms_);
    }
    remember(coid, OrderMeta{"STOP_LEG", sym, "", env.msg_id, res.order.id});
    publish_submitted(env, p, res, "stop");
    spdlog::info("execution: STOP_REPLACE {} -> {} at {}", sym, res.order.id, cents_to_decimal(new_stop));
}

void ExecutionEngine::publish_submitted(const Envelope& env, const nlohmann::json& p, const SubmitResult& r, const std::string& order_type) {
    nlohmann::json legs = nlohmann::json::array();
    for (const auto& l : r.order.legs)
        legs.push_back({{"broker_order_id", l.id}, {"leg_type", l.type == "limit" ? "take_profit" : "stop_loss"}, {"px_cents", l.stop_px_cents ? nlohmann::json(*l.stop_px_cents) : (l.limit_px_cents ? nlohmann::json(*l.limit_px_cents) : nlohmann::json(nullptr))}});
    nlohmann::json s = {
        {"client_order_id", p["client_order_id"]}, {"broker_order_id", r.order.id}, {"approved_msg_id", env.msg_id}, {"symbol", p["symbol"]}, {"side", p.value("side", r.order.side)},
        {"qty", p["qty"]}, {"order_type", order_type}, {"limit_px_cents", r.order.limit_px_cents ? nlohmann::json(*r.order.limit_px_cents) : nlohmann::json(nullptr)},
        {"stop_px_cents", r.order.stop_px_cents ? nlohmann::json(*r.order.stop_px_cents) : (p.contains("stop_px_cents") ? p["stop_px_cents"] : nlohmann::json(nullptr))},
        {"submitted_ts_utc", now_utc_iso()}, {"legs", legs}, {"attempt", r.attempts}, {"deduped_at_broker", r.deduped_at_broker},
    };
    bus_.publish("orders.submitted", make_envelope("execution", s, env.msg_id));
}

void ExecutionEngine::on_trade_update(const nlohmann::json& data) {
    if (!data.contains("order")) return;
    AlpacaOrder o = AlpacaOrder::from_json(data["order"]);
    std::string event = data.value("event", "");
    if (!o.parent_id && o.legs.empty() == false) {
        for (const auto& leg : o.legs) if (!by_coid_.count(leg.client_order_id)) remember(leg.client_order_id, OrderMeta{leg.type == "limit" ? "TAKE_PROFIT_LEG" : "STOP_LEG", o.symbol, "", "", leg.id});
    }
    if (!broker_to_coid_.count(o.id) && by_coid_.count(o.client_order_id)) { by_coid_[o.client_order_id].broker_order_id = o.id; broker_to_coid_[o.id] = o.client_order_id; }
    nlohmann::json st = {
        {"client_order_id", o.client_order_id}, {"broker_order_id", o.id}, {"parent_broker_order_id", o.parent_id ? nlohmann::json(*o.parent_id) : nlohmann::json(nullptr)},
        {"symbol", o.symbol}, {"side", o.side}, {"event", event.empty() ? "accepted" : event}, {"status", o.status}, {"qty", o.qty}, {"filled_qty", o.filled_qty},
        {"filled_avg_px_cents", o.filled_avg_px_cents ? nlohmann::json(*o.filled_avg_px_cents) : nlohmann::json(nullptr)},
        {"price_cents", data.contains("price") ? nlohmann::json(alpaca_cents(data["price"])) : nlohmann::json(nullptr)},
        {"position_qty", data.contains("position_qty") ? nlohmann::json(static_cast<std::int64_t>(std::stod(data["position_qty"].is_string() ? data["position_qty"].get<std::string>() : std::to_string(data["position_qty"].get<double>())))) : nlohmann::json(nullptr)},
        {"event_ts_utc", data.value("timestamp", now_utc_iso())}, {"raw", data},
    };
    bus_.publish("orders.status", make_envelope("execution", st));
    if (event == "fill" && o.status == "filled" && !filled_emitted_.count(o.id)) {
        filled_emitted_.insert(o.id);
        auto mit = by_coid_.find(o.client_order_id);
        nlohmann::json f = {
            {"client_order_id", o.client_order_id}, {"broker_order_id", o.id}, {"parent_broker_order_id", o.parent_id ? nlohmann::json(*o.parent_id) : nlohmann::json(nullptr)},
            {"symbol", o.symbol}, {"side", o.side}, {"qty", o.filled_qty > 0 ? o.filled_qty : o.qty}, {"avg_px_cents", o.filled_avg_px_cents.value_or(alpaca_cents(data.value("price", nlohmann::json("0"))))},
            {"filled_ts_utc", data.value("timestamp", now_utc_iso())}, {"intent", intent_for(o)},
            {"candidate_msg_id", mit != by_coid_.end() && !mit->second.candidate_msg_id.empty() ? nlohmann::json(mit->second.candidate_msg_id) : nlohmann::json(nullptr)},
            {"commission_cents", 0}, {"fees_cents", 0},
        };
        bus_.publish("orders.filled", make_envelope("execution", f));
        save_state();
    }
    if (event == "rejected") spdlog::warn("execution: broker rejected {} ({})", o.client_order_id, data.dump().substr(0, 300));
}

nlohmann::json ExecutionEngine::state_json() const {
    nlohmann::json m = nlohmann::json::object();
    for (const auto& [k, v] : by_coid_) m[k] = v.to_json();
    return {{"by_coid", m}, {"filled_emitted", std::vector<std::string>(filled_emitted_.begin(), filled_emitted_.end())}, {"halted", halted_}};
}

void ExecutionEngine::load_state(const nlohmann::json& j) {
    by_coid_.clear();
    broker_to_coid_.clear();
    for (auto& [k, v] : json_obj(j, "by_coid").items()) {
        by_coid_[k] = OrderMeta::from_json(v);
        if (!by_coid_[k].broker_order_id.empty()) broker_to_coid_[by_coid_[k].broker_order_id] = k;
    }
    filled_emitted_.clear();
    for (const auto& s : j.value("filled_emitted", nlohmann::json::array())) filled_emitted_.insert(s.get<std::string>());
    halted_ = j.value("halted", false);
}

void ExecutionEngine::save_state() const {
    if (state_file_.empty()) return;
    std::filesystem::create_directories(state_file_.parent_path());
    std::ofstream out(state_file_.string() + ".tmp");
    out << state_json().dump();
    out.close();
    std::filesystem::rename(state_file_.string() + ".tmp", state_file_);
}

} // namespace at
