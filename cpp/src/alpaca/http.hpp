// Small libcurl wrapper with a token-bucket rate limiter (Alpaca: 200 req/min).
#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace at {

struct HttpResponse {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    bool ok() const { return status >= 200 && status < 300; }
};

class RateLimiter {
public:
    explicit RateLimiter(int per_minute) : capacity_(per_minute), tokens_(per_minute), refill_per_s_(per_minute / 60.0) {}
    // Blocks until a token is available.
    void acquire();
private:
    double capacity_;
    double tokens_;
    double refill_per_s_;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
    std::mutex m_;
};

class HttpClient {
public:
    HttpClient(std::map<std::string, std::string> default_headers, int timeout_s = 10, int rate_per_minute = 200);
    ~HttpClient();
    HttpResponse request(const std::string& method, const std::string& url, const std::string& body = "");
    HttpResponse get(const std::string& url) { return request("GET", url); }
    HttpResponse post(const std::string& url, const std::string& body) { return request("POST", url, body); }
    HttpResponse patch(const std::string& url, const std::string& body) { return request("PATCH", url, body); }
    HttpResponse del(const std::string& url) { return request("DELETE", url); }
private:
    std::map<std::string, std::string> headers_;
    int timeout_s_;
    RateLimiter limiter_;
};

std::string url_encode(const std::string& s);

} // namespace at
