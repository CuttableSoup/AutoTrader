#include "bus.hpp"

#include <sstream>

namespace at {

namespace {
std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '.') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}
} // namespace

bool subject_matches(const std::string& filter, const std::string& subject) {
    auto f = tokens(filter);
    auto s = tokens(subject);
    std::size_t i = 0;
    for (; i < f.size(); ++i) {
        if (f[i] == ">") return i < s.size();
        if (i >= s.size()) return false;
        if (f[i] != "*" && f[i] != s[i]) return false;
    }
    return i == s.size();
}

bool Dedupe::first_time(const std::string& msg_id) {
    if (seen_.count(msg_id)) return false;
    seen_.insert(msg_id);
    order_.push_back(msg_id);
    while (order_.size() > capacity_) {
        seen_.erase(order_.front());
        order_.pop_front();
    }
    return true;
}

void MemoryBus::publish(const std::string& subject, const Envelope& env) {
    std::lock_guard<std::mutex> lock(m_);
    ++seq_;
    log_.emplace_back(subject, env);
    for (std::size_t i = 0; i < subs_.size(); ++i) {
        if (!subject_matches(subs_[i].filter, subject)) continue;
        queue_.push_back(Pending{i, Delivery{subject, env, seq_, 0}});
        if (redeliver_) queue_.push_back(Pending{i, Delivery{subject, env, seq_, 1}});
    }
}

void MemoryBus::subscribe(const std::string& subject_filter, const std::string& durable, Handler handler) {
    std::lock_guard<std::mutex> lock(m_);
    subs_.push_back(Sub{subject_filter, durable, std::move(handler)});
}

std::size_t MemoryBus::poll() {
    std::deque<Pending> batch;
    {
        std::lock_guard<std::mutex> lock(m_);
        batch.swap(queue_);
    }
    std::size_t n = 0;
    for (auto& p : batch) {
        try {
            subs_[p.sub_index].handler(p.delivery);
        } catch (...) {
            // nak: redeliver once more at the back of the queue
            if (p.delivery.redelivery_count < 3) {
                std::lock_guard<std::mutex> lock(m_);
                Pending again = p;
                ++again.delivery.redelivery_count;
                queue_.push_back(std::move(again));
            }
        }
        ++n;
    }
    return n;
}

std::size_t MemoryBus::drain(std::size_t max_rounds) {
    std::size_t total = 0;
    for (std::size_t r = 0; r < max_rounds; ++r) {
        std::size_t n = poll();
        if (n == 0) break;
        total += n;
    }
    return total;
}

std::vector<Envelope> MemoryBus::published(const std::string& subject_filter) const {
    std::vector<Envelope> out;
    for (const auto& [subj, env] : log_)
        if (subject_matches(subject_filter, subj)) out.push_back(env);
    return out;
}

} // namespace at
