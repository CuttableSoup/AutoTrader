// Secrets provider. Backends: env (AT_<KEY>), file (JSON, dev only),
// aws-secretsmanager (via the aws CLI so the core has no AWS SDK dependency).
// Keys are logical names: alpaca_key_id, alpaca_secret_key, anthropic_api_key,
// fmp_api_key, finnhub_api_key, watchdog_token.
#pragma once
#include "config.hpp"

#include <map>
#include <string>

namespace at {

class Secrets {
public:
    static Secrets load(const Config& cfg);
    static Secrets from_map(std::map<std::string, std::string> kv) { Secrets s; s.kv_ = std::move(kv); return s; }

    // Throws std::runtime_error if absent or empty.
    std::string require(const std::string& key) const;
    std::string get(const std::string& key, const std::string& def = "") const;
    bool has(const std::string& key) const;
    std::string backend() const { return backend_; }

private:
    std::map<std::string, std::string> kv_;
    std::string backend_;
};

} // namespace at
