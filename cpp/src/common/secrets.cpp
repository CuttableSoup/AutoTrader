#include "secrets.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace at {

namespace {

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const char* kKeys[] = {"alpaca_key_id", "alpaca_secret_key", "anthropic_api_key", "fmp_api_key",
                       "finnhub_api_key", "nasdaq_data_link_api_key", "watchdog_token"};

std::string run_capture(const std::string& cmd) {
#ifdef _WIN32
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(_popen(cmd.c_str(), "r"), _pclose);
#else
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
#endif
    if (!pipe) throw std::runtime_error("secrets: cannot run aws cli");
    std::array<char, 4096> buf{};
    std::string out;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe.get())) out += buf.data();
    return out;
}

} // namespace

Secrets Secrets::load(const Config& cfg) {
    Secrets s;
    s.backend_ = cfg.get<std::string>("secrets.backend", "env");
    if (s.backend_ == "env") {
        for (const char* k : kKeys) {
            std::string env_name = "AT_" + upper(k);
            if (const char* v = std::getenv(env_name.c_str()); v && *v) s.kv_[k] = v;
        }
    } else if (s.backend_ == "file") {
        auto path = cfg.resolve(cfg.get<std::string>("secrets.file", "config/secrets.json"));
        std::ifstream in(path);
        if (!in) throw std::runtime_error("secrets: cannot open " + path.string());
        nlohmann::json j;
        in >> j;
        for (auto& [k, v] : j.items())
            if (v.is_string() && !k.starts_with("_")) s.kv_[k] = v.get<std::string>();
    } else if (s.backend_ == "aws-secretsmanager") {
        auto name = cfg.require<std::string>("secrets.aws_secret_name");
        std::string out = run_capture("aws secretsmanager get-secret-value --secret-id \"" + name +
                                      "\" --query SecretString --output text");
        auto j = nlohmann::json::parse(out);
        for (auto& [k, v] : j.items())
            if (v.is_string()) s.kv_[k] = v.get<std::string>();
    } else {
        throw std::runtime_error("secrets: unknown backend '" + s.backend_ + "'");
    }
    spdlog::info("secrets: backend={} keys_loaded={}", s.backend_, s.kv_.size());
    return s;
}

std::string Secrets::require(const std::string& key) const {
    auto it = kv_.find(key);
    if (it == kv_.end() || it->second.empty()) throw std::runtime_error("secrets: missing '" + key + "' (backend " + backend_ + ")");
    return it->second;
}

std::string Secrets::get(const std::string& key, const std::string& def) const {
    auto it = kv_.find(key);
    return it == kv_.end() ? def : it->second;
}

bool Secrets::has(const std::string& key) const {
    auto it = kv_.find(key);
    return it != kv_.end() && !it->second.empty();
}

} // namespace at
