// Alpaca trade_updates WebSocket (authoritative order-state feed). Messages are
// queued on the socket thread and drained by the service's main loop.
#pragma once
#include <nlohmann/json.hpp>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace at {

class TradeUpdatesStream {
public:
    TradeUpdatesStream(std::string ws_url, std::string key_id, std::string secret_key);
    ~TradeUpdatesStream();
    void start();
    void stop();
    bool connected() const { return connected_.load(); }
    bool listening() const { return listening_.load(); }
    // Drain queued trade_updates payloads (the "data" object of each message).
    std::vector<nlohmann::json> drain();
    std::size_t reconnects() const { return reconnects_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string url_, key_, secret_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> listening_{false};
    std::atomic<std::size_t> reconnects_{0};
    std::mutex m_;
    std::deque<nlohmann::json> queue_;
};

} // namespace at
