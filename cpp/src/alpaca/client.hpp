// Hand-rolled Alpaca trading + market data REST client. Money in cents,
// decimals parsed exactly. Deprecated PDT fields are never read.
#pragma once
#include "alpaca/http.hpp"
#include "common/money.hpp"
#include "common/time.hpp"
#include "strategy/types.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace at {

struct AlpacaAccount {
    std::string id;
    std::string status;
    Cents equity_cents = 0;
    Cents cash_cents = 0;
    Cents buying_power_cents = 0;            // non_marginable_buying_power
    Cents last_equity_cents = 0;
    bool trading_blocked = false;
    nlohmann::json raw;
};

struct AlpacaPosition {
    std::string symbol;
    std::int64_t qty = 0;
    Cents avg_entry_px_cents = 0;
    Cents current_px_cents = 0;
    Cents market_value_cents = 0;
    nlohmann::json raw;
};

struct AlpacaOrder {
    std::string id;
    std::string client_order_id;
    std::string symbol;
    std::string side;
    std::string type;
    std::string status;
    std::string tif;
    std::string order_class;
    std::int64_t qty = 0;
    std::int64_t filled_qty = 0;
    std::optional<Cents> limit_px_cents;
    std::optional<Cents> stop_px_cents;
    std::optional<Cents> filled_avg_px_cents;
    std::optional<std::string> parent_id;
    std::vector<AlpacaOrder> legs;
    std::string created_at;
    nlohmann::json raw;
    static AlpacaOrder from_json(const nlohmann::json& j);
};

struct OrderRequest {
    std::string symbol;
    std::int64_t qty = 0;
    std::string side;                 // buy | sell
    std::string type = "market";      // market | limit | stop | stop_limit
    std::string tif = "day";          // day | gtc
    std::optional<Cents> limit_px_cents;
    std::optional<Cents> stop_px_cents;
    std::string client_order_id;
    std::string order_class;          // "" | oto | bracket
    std::optional<Cents> stop_loss_px_cents;    // leg
    std::optional<Cents> take_profit_px_cents;  // leg
    nlohmann::json to_json() const;
};

struct SubmitResult {
    AlpacaOrder order;
    bool deduped_at_broker = false;   // broker already had this client_order_id
    int attempts = 1;
};

class AlpacaError : public std::runtime_error {
public:
    AlpacaError(long status, std::string body) : std::runtime_error("alpaca " + std::to_string(status) + ": " + body.substr(0, 300)), status(status), body(std::move(body)) {}
    long status;
    std::string body;
};

class AlpacaClient {
public:
    AlpacaClient(std::string trading_base_url, std::string data_base_url, const std::string& key_id, const std::string& secret_key,
                 int timeout_s = 10, int rate_per_minute = 200, std::string data_feed = "iex");

    AlpacaAccount account();
    std::vector<AlpacaPosition> positions();
    std::vector<AlpacaOrder> orders(const std::string& status = "open", bool nested = true);
    AlpacaOrder order(const std::string& id);
    std::optional<AlpacaOrder> order_by_client_id(const std::string& client_order_id);
    // Idempotent: a 422 "client_order_id must be unique" is resolved by fetching the existing order.
    SubmitResult submit(const OrderRequest& req, int max_attempts = 3, const std::vector<int>& backoff_ms = {500, 1500, 4000});
    AlpacaOrder replace(const std::string& id, std::optional<std::int64_t> qty, std::optional<Cents> limit_px, std::optional<Cents> stop_px, const std::string& new_client_order_id = "");
    void cancel(const std::string& id);
    void cancel_all();
    void close_all(bool cancel_orders = true);
    void close_position(const std::string& symbol);

    struct Clock { bool is_open = false; std::string timestamp; std::string next_open; std::string next_close; };
    Clock clock();
    std::vector<Date> calendar(Date start, Date end);

    // Daily bars, split-adjusted, [start, end]. Returns symbol -> bars ascending.
    std::map<std::string, BarSeries> daily_bars(const std::vector<std::string>& symbols, Date start, Date end);
    std::map<std::string, Quote> latest_quotes(const std::vector<std::string>& symbols);

    const std::string& trading_base_url() const { return trading_; }

private:
    nlohmann::json call(const std::string& method, const std::string& url, const std::string& body = "");
    std::string trading_;
    std::string data_;
    std::string feed_;
    std::unique_ptr<HttpClient> http_;
};

// Alpaca decimals arrive as strings ("123.45") or numbers; both handled.
Cents alpaca_cents(const nlohmann::json& v);
std::optional<Cents> alpaca_opt_cents(const nlohmann::json& j, const char* key);

} // namespace at
