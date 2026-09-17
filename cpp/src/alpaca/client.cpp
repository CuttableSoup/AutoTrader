#include "alpaca/client.hpp"

#include "common/json_util.hpp"
#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace at {

Cents alpaca_cents(const nlohmann::json& v) {
    if (v.is_null()) return 0;
    if (v.is_string()) return parse_cents(v.get<std::string>());
    if (v.is_number_integer()) return v.get<std::int64_t>() * 100;
    return cents_from_dollars(v.get<double>());
}

std::optional<Cents> alpaca_opt_cents(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return std::nullopt;
    return alpaca_cents(j[key]);
}

namespace {
std::int64_t qty_of(const nlohmann::json& v) {
    if (v.is_null()) return 0;
    if (v.is_string()) return static_cast<std::int64_t>(std::stod(v.get<std::string>()));
    return static_cast<std::int64_t>(v.get<double>());
}
std::string str_or(const nlohmann::json& j, const char* k, const std::string& def = "") {
    return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : def;
}
} // namespace

AlpacaOrder AlpacaOrder::from_json(const nlohmann::json& j) {
    AlpacaOrder o;
    o.id = str_or(j, "id");
    o.client_order_id = str_or(j, "client_order_id");
    o.symbol = str_or(j, "symbol");
    o.side = str_or(j, "side");
    o.type = str_or(j, "type", str_or(j, "order_type"));
    o.status = str_or(j, "status");
    o.tif = str_or(j, "time_in_force");
    o.order_class = str_or(j, "order_class", "simple");
    o.qty = j.contains("qty") ? qty_of(j["qty"]) : 0;
    o.filled_qty = j.contains("filled_qty") ? qty_of(j["filled_qty"]) : 0;
    o.limit_px_cents = alpaca_opt_cents(j, "limit_price");
    o.stop_px_cents = alpaca_opt_cents(j, "stop_price");
    o.filled_avg_px_cents = alpaca_opt_cents(j, "filled_avg_price");
    if (j.contains("parent_id") && j["parent_id"].is_string()) o.parent_id = j["parent_id"].get<std::string>();
    if (j.contains("legs") && j["legs"].is_array())
        for (const auto& l : j["legs"]) o.legs.push_back(from_json(l));
    o.created_at = str_or(j, "created_at");
    o.raw = j;
    return o;
}

nlohmann::json OrderRequest::to_json() const {
    nlohmann::json j = {{"symbol", symbol}, {"qty", std::to_string(qty)}, {"side", side}, {"type", type}, {"time_in_force", tif}, {"client_order_id", client_order_id}, {"extended_hours", false}};
    if (limit_px_cents) j["limit_price"] = cents_to_decimal(*limit_px_cents);
    if (stop_px_cents) j["stop_price"] = cents_to_decimal(*stop_px_cents);
    if (!order_class.empty()) j["order_class"] = order_class;
    if (stop_loss_px_cents) j["stop_loss"] = {{"stop_price", cents_to_decimal(*stop_loss_px_cents)}};
    if (take_profit_px_cents) j["take_profit"] = {{"limit_price", cents_to_decimal(*take_profit_px_cents)}};
    return j;
}

AlpacaClient::AlpacaClient(std::string trading_base_url, std::string data_base_url, const std::string& key_id, const std::string& secret_key, int timeout_s, int rate_per_minute, std::string data_feed)
    : trading_(std::move(trading_base_url)), data_(std::move(data_base_url)), feed_(std::move(data_feed)) {
    while (!trading_.empty() && trading_.back() == '/') trading_.pop_back();
    while (!data_.empty() && data_.back() == '/') data_.pop_back();
    http_ = std::make_unique<HttpClient>(std::map<std::string, std::string>{{"APCA-API-KEY-ID", key_id}, {"APCA-API-SECRET-KEY", secret_key}, {"Content-Type", "application/json"}, {"Accept", "application/json"}}, timeout_s, rate_per_minute);
}

nlohmann::json AlpacaClient::call(const std::string& method, const std::string& url, const std::string& body) {
    HttpResponse r = http_->request(method, url, body);
    if (!r.ok()) throw AlpacaError(r.status, r.body);
    if (r.body.empty()) return nlohmann::json();
    return nlohmann::json::parse(r.body);
}

AlpacaAccount AlpacaClient::account() {
    auto j = call("GET", trading_ + "/v2/account");
    AlpacaAccount a;
    a.id = str_or(j, "id");
    a.status = str_or(j, "status");
    a.equity_cents = alpaca_cents(j.value("equity", nlohmann::json("0")));
    a.cash_cents = alpaca_cents(j.value("cash", nlohmann::json("0")));
    a.buying_power_cents = alpaca_cents(j.contains("non_marginable_buying_power") ? j["non_marginable_buying_power"] : j.value("buying_power", nlohmann::json("0")));
    a.marginable_buying_power_cents = alpaca_cents(j.value("buying_power", nlohmann::json("0")));
    a.last_equity_cents = alpaca_cents(j.value("last_equity", nlohmann::json("0")));
    a.trading_blocked = j.value("trading_blocked", false) || j.value("account_blocked", false);
    a.raw = j;
    return a;
}

std::vector<AlpacaPosition> AlpacaClient::positions() {
    auto j = call("GET", trading_ + "/v2/positions");
    std::vector<AlpacaPosition> out;
    for (const auto& p : j) {
        AlpacaPosition x;
        x.symbol = str_or(p, "symbol");
        x.qty = qty_of(p.value("qty", nlohmann::json("0")));
        x.avg_entry_px_cents = alpaca_cents(p.value("avg_entry_price", nlohmann::json("0")));
        x.current_px_cents = alpaca_cents(p.value("current_price", nlohmann::json("0")));
        x.market_value_cents = alpaca_cents(p.value("market_value", nlohmann::json("0")));
        x.raw = p;
        out.push_back(std::move(x));
    }
    return out;
}

std::vector<AlpacaOrder> AlpacaClient::orders(const std::string& status, bool nested) {
    auto j = call("GET", trading_ + "/v2/orders?status=" + status + "&limit=500&nested=" + (nested ? "true" : "false"));
    std::vector<AlpacaOrder> out;
    for (const auto& o : j) out.push_back(AlpacaOrder::from_json(o));
    return out;
}

AlpacaOrder AlpacaClient::order(const std::string& id) { return AlpacaOrder::from_json(call("GET", trading_ + "/v2/orders/" + id)); }

std::optional<AlpacaOrder> AlpacaClient::order_by_client_id(const std::string& coid) {
    try {
        return AlpacaOrder::from_json(call("GET", trading_ + "/v2/orders:by_client_order_id?client_order_id=" + url_encode(coid)));
    } catch (const AlpacaError& e) {
        if (e.status == 404) return std::nullopt;
        throw;
    }
}

SubmitResult AlpacaClient::submit(const OrderRequest& req, int max_attempts, const std::vector<int>& backoff_ms) {
    SubmitResult res;
    std::string body = req.to_json().dump();
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        res.attempts = attempt;
        try {
            res.order = AlpacaOrder::from_json(call("POST", trading_ + "/v2/orders", body));
            return res;
        } catch (const AlpacaError& e) {
            if (e.status == 422 && e.body.find("client_order_id") != std::string::npos) {
                // A previous attempt reached the broker but the response was lost: fetch it. Idempotent by construction.
                if (auto existing = order_by_client_id(req.client_order_id)) { res.order = *existing; res.deduped_at_broker = true; return res; }
            }
            if (e.status >= 400 && e.status < 500 && e.status != 429) throw;   // rejected: do not retry
            spdlog::warn("submit {} attempt {} failed: {}", req.client_order_id, attempt, e.what());
        } catch (const std::exception& e) {
            spdlog::warn("submit {} attempt {} transport error: {}", req.client_order_id, attempt, e.what());
            // Response may have been lost after the broker accepted it: check before retrying.
            try {
                if (auto existing = order_by_client_id(req.client_order_id)) { res.order = *existing; res.deduped_at_broker = true; return res; }
            } catch (const std::exception& e2) { spdlog::warn("lookup after transport error failed: {}", e2.what()); }
        }
        if (attempt < max_attempts) {
            int ms = attempt - 1 < static_cast<int>(backoff_ms.size()) ? backoff_ms[static_cast<std::size_t>(attempt - 1)] : backoff_ms.back();
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }
    throw std::runtime_error("submit " + req.client_order_id + ": all " + std::to_string(max_attempts) + " attempts failed");
}

AlpacaOrder AlpacaClient::replace(const std::string& id, std::optional<std::int64_t> qty, std::optional<Cents> limit_px, std::optional<Cents> stop_px, const std::string& new_coid) {
    nlohmann::json j = nlohmann::json::object();
    if (qty) j["qty"] = std::to_string(*qty);
    if (limit_px) j["limit_price"] = cents_to_decimal(*limit_px);
    if (stop_px) j["stop_price"] = cents_to_decimal(*stop_px);
    if (!new_coid.empty()) j["client_order_id"] = new_coid;
    return AlpacaOrder::from_json(call("PATCH", trading_ + "/v2/orders/" + id, j.dump()));
}

void AlpacaClient::cancel(const std::string& id) {
    try { call("DELETE", trading_ + "/v2/orders/" + id); }
    catch (const AlpacaError& e) { if (e.status != 404 && e.status != 422) throw; }
}

void AlpacaClient::cancel_all() { call("DELETE", trading_ + "/v2/orders"); }
void AlpacaClient::close_all(bool cancel_orders) { call("DELETE", trading_ + "/v2/positions?cancel_orders=" + std::string(cancel_orders ? "true" : "false")); }
void AlpacaClient::close_position(const std::string& symbol) { call("DELETE", trading_ + "/v2/positions/" + symbol); }

AlpacaClient::Clock AlpacaClient::clock() {
    auto j = call("GET", trading_ + "/v2/clock");
    Clock c;
    c.is_open = j.value("is_open", false);
    c.timestamp = str_or(j, "timestamp");
    c.next_open = str_or(j, "next_open");
    c.next_close = str_or(j, "next_close");
    return c;
}

std::vector<Date> AlpacaClient::calendar(Date start, Date end) {
    auto j = call("GET", trading_ + "/v2/calendar?start=" + iso_date(start) + "&end=" + iso_date(end));
    std::vector<Date> out;
    for (const auto& d : j) if (auto dd = parse_date(str_or(d, "date"))) out.push_back(*dd);
    return out;
}

std::map<std::string, BarSeries> AlpacaClient::daily_bars(const std::vector<std::string>& symbols, Date start, Date end, const std::string& adjustment) {
    std::map<std::string, BarSeries> out;
    for (std::size_t i = 0; i < symbols.size(); i += 100) {
        std::string syms;
        for (std::size_t k = i; k < std::min(symbols.size(), i + 100); ++k) { if (!syms.empty()) syms += ','; syms += symbols[k]; }
        std::string token;
        do {
            std::string url = data_ + "/v2/stocks/bars?symbols=" + url_encode(syms) + "&timeframe=1Day&adjustment=" + adjustment + "&feed=" + feed_ + "&limit=10000&start=" + iso_date(start) + "&end=" + iso_date(end);
            if (!token.empty()) url += "&page_token=" + url_encode(token);
            auto j = call("GET", url);
            for (auto& [sym, arr] : json_obj(j, "bars").items()) {
                for (const auto& b : arr) {
                    Bar bar;
                    auto t = parse_iso_utc(str_or(b, "t"));
                    if (!t) continue;
                    bar.date = to_ny(*t).date;
                    bar.open = alpaca_cents(b["o"]); bar.high = alpaca_cents(b["h"]); bar.low = alpaca_cents(b["l"]); bar.close = alpaca_cents(b["c"]);
                    bar.volume = qty_of(b["v"]);
                    out[sym].push_back(bar);
                }
            }
            token = j.contains("next_page_token") && j["next_page_token"].is_string() ? j["next_page_token"].get<std::string>() : "";
        } while (!token.empty());
    }
    for (auto& [s, v] : out) std::sort(v.begin(), v.end(), [](const Bar& a, const Bar& b) { return a.date < b.date; });
    return out;
}

std::map<std::string, bool> AlpacaClient::shortable_flags(const std::vector<std::string>& symbols) {
    std::map<std::string, bool> out;
    for (const auto& sym : symbols) {
        try {
            auto j = call("GET", trading_ + "/v2/assets/" + url_encode(sym));
            out[sym] = j.value("shortable", false) && j.value("easy_to_borrow", false);
        } catch (const std::exception& e) {
            spdlog::warn("alpaca: shortable lookup for {} failed: {}", sym, e.what());
        }
    }
    return out;
}

std::map<std::string, Quote> AlpacaClient::latest_quotes(const std::vector<std::string>& symbols) {
    std::map<std::string, Quote> out;
    for (std::size_t i = 0; i < symbols.size(); i += 100) {
        std::string syms;
        for (std::size_t k = i; k < std::min(symbols.size(), i + 100); ++k) { if (!syms.empty()) syms += ','; syms += symbols[k]; }
        auto j = call("GET", data_ + "/v2/stocks/quotes/latest?symbols=" + url_encode(syms) + "&feed=" + feed_);
        for (auto& [sym, q] : json_obj(j, "quotes").items()) {
            Quote x;
            x.bid_cents = alpaca_cents(q.value("bp", nlohmann::json(0)));
            x.ask_cents = alpaca_cents(q.value("ap", nlohmann::json(0)));
            x.bid_size = qty_of(q.value("bs", nlohmann::json(0)));
            x.ask_size = qty_of(q.value("as", nlohmann::json(0)));
            if (auto t = parse_iso_utc(str_or(q, "t"))) x.ts = *t;
            out[sym] = x;
        }
    }
    return out;
}

} // namespace at
