#include "alpaca/trade_stream.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <spdlog/spdlog.h>

namespace at {

struct TradeUpdatesStream::Impl {
    ix::WebSocket ws;
};

TradeUpdatesStream::TradeUpdatesStream(std::string ws_url, std::string key_id, std::string secret_key)
    : impl_(std::make_unique<Impl>()), url_(std::move(ws_url)), key_(std::move(key_id)), secret_(std::move(secret_key)) {
    ix::initNetSystem();
}

TradeUpdatesStream::~TradeUpdatesStream() {
    stop();
    ix::uninitNetSystem();
}

void TradeUpdatesStream::start() {
    impl_->ws.setUrl(url_);
    impl_->ws.setPingInterval(20);
    impl_->ws.enableAutomaticReconnection();
    impl_->ws.setMinWaitBetweenReconnectionRetries(1000);
    impl_->ws.setMaxWaitBetweenReconnectionRetries(15000);
    impl_->ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open: {
                connected_ = true;
                nlohmann::json auth = {{"action", "auth"}, {"key", key_}, {"secret", secret_}};
                impl_->ws.send(auth.dump());
                spdlog::info("trade_updates: connected, authenticating");
                break;
            }
            case ix::WebSocketMessageType::Message: {
                try {
                    auto j = nlohmann::json::parse(msg->str);
                    std::string stream = j.value("stream", "");
                    if (stream == "authorization") {
                        std::string status = j["data"].value("status", "");
                        if (status == "authorized") {
                            nlohmann::json listen = {{"action", "listen"}, {"data", {{"streams", {"trade_updates"}}}}};
                            impl_->ws.send(listen.dump());
                        } else {
                            spdlog::error("trade_updates: authorization failed: {}", msg->str);
                        }
                    } else if (stream == "listening") {
                        listening_ = true;
                        spdlog::info("trade_updates: listening");
                    } else if (stream == "trade_updates") {
                        std::lock_guard<std::mutex> lock(m_);
                        queue_.push_back(j["data"]);
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("trade_updates: bad message: {}", e.what());
                }
                break;
            }
            case ix::WebSocketMessageType::Close:
                connected_ = false;
                listening_ = false;
                spdlog::warn("trade_updates: closed ({}): {}", msg->closeInfo.code, msg->closeInfo.reason);
                break;
            case ix::WebSocketMessageType::Error:
                connected_ = false;
                listening_ = false;
                ++reconnects_;
                spdlog::warn("trade_updates: error {} (retries {})", msg->errorInfo.reason, msg->errorInfo.retries);
                break;
            default:
                break;
        }
    });
    impl_->ws.start();
}

void TradeUpdatesStream::stop() {
    impl_->ws.stop();
    connected_ = false;
    listening_ = false;
}

std::vector<nlohmann::json> TradeUpdatesStream::drain() {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<nlohmann::json> out(queue_.begin(), queue_.end());
    queue_.clear();
    return out;
}

} // namespace at
