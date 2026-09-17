// Execution engine: orders.approved -> Alpaca -> orders.submitted / orders.status / orders.filled.
// Every broker order carries the deterministic client_order_id from the risk
// manager; retries dedupe at the broker. control.flatten cancels everything
// and closes everything; control.halt stops all new submissions.
#pragma once
#include "alpaca/client.hpp"
#include "common/bus.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace at {

struct OrderMeta {
    std::string intent;               // ENTRY, EXIT_TIME, ..., STOP_LEG, TAKE_PROFIT_LEG, REBALANCE_TO_WEIGHT
    std::string symbol;
    std::string candidate_msg_id;     // may be empty
    std::string approved_msg_id;
    std::string broker_order_id;
    // REBALANCE_TO_WEIGHT only: carried through to the orders.filled payload so the
    // portfolio service can call Ledger::apply_rebalance_fill with the right signed target
    // and asset_class, rather than the earnings-shaped apply_entry_fill/apply_exit_fill pair.
    std::optional<std::int64_t> target_qty;
    std::string asset_class;
    nlohmann::json to_json() const;
    static OrderMeta from_json(const nlohmann::json& j);
};

class ExecutionEngine {
public:
    ExecutionEngine(AlpacaClient& client, IBus& bus, std::filesystem::path state_file, int max_attempts = 3, std::vector<int> backoff_ms = {500, 1500, 4000});

    void on_approved(const Envelope& env);
    void on_control(const std::string& subject, const nlohmann::json& payload);
    // One trade_updates message (the "data" object). Publishes orders.status and, on terminal fill, orders.filled.
    void on_trade_update(const nlohmann::json& data);

    bool halted() const { return halted_; }
    nlohmann::json state_json() const;
    void load_state(const nlohmann::json& j);
    void save_state() const;

private:
    void submit_entry(const Envelope& env, const nlohmann::json& p);
    void submit_exit(const Envelope& env, const nlohmann::json& p);
    void submit_rebalance(const Envelope& env, const nlohmann::json& p);
    void replace_stop(const Envelope& env, const nlohmann::json& p);
    void publish_submitted(const Envelope& env, const nlohmann::json& p, const SubmitResult& r, const std::string& order_type);
    void remember(const std::string& coid, const OrderMeta& m);
    std::string intent_for(const AlpacaOrder& o) const;

    AlpacaClient& client_;
    IBus& bus_;
    std::filesystem::path state_file_;
    int max_attempts_;
    std::vector<int> backoff_ms_;
    std::map<std::string, OrderMeta> by_coid_;
    std::map<std::string, std::string> broker_to_coid_;
    std::set<std::string> filled_emitted_;
    Dedupe seen_{200000};
    bool halted_ = false;
};

} // namespace at
