#include "alpaca/http.hpp"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <thread>

namespace at {

void RateLimiter::acquire() {
    std::lock_guard<std::mutex> lock(m_);
    for (;;) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        tokens_ = std::min(capacity_, tokens_ + dt * refill_per_s_);
        if (tokens_ >= 1.0) { tokens_ -= 1.0; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

namespace {
struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};
CurlGlobal& curl_global() { static CurlGlobal g; return g; }

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}
size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    std::string line(buffer, size * nitems);
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string k = line.substr(0, colon), v = line.substr(colon + 1);
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return std::tolower(c); });
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        (*headers)[k] = v;
    }
    return size * nitems;
}
} // namespace

HttpClient::HttpClient(std::map<std::string, std::string> default_headers, int timeout_s, int rate_per_minute)
    : headers_(std::move(default_headers)), timeout_s_(timeout_s), limiter_(rate_per_minute) {
    curl_global();
}

HttpClient::~HttpClient() = default;

HttpResponse HttpClient::request(const std::string& method, const std::string& url, const std::string& body) {
    limiter_.acquire();
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init failed");
    HttpResponse r;
    struct curl_slist* hl = nullptr;
    for (const auto& [k, v] : headers_) hl = curl_slist_append(hl, (k + ": " + v).c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s_));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    if (method == "POST") {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (method != "GET") {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        std::string err = curl_easy_strerror(rc);
        curl_slist_free_all(hl);
        curl_easy_cleanup(c);
        throw std::runtime_error("http " + method + " " + url + ": " + err);
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    spdlog::debug("http {} {} -> {} ({} bytes)", method, url, r.status, r.body.size());
    return r;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += static_cast<char>(ch);
        else { out += '%'; out += hex[ch >> 4]; out += hex[ch & 15]; }
    }
    return out;
}

} // namespace at
