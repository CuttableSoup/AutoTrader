// Bus abstraction. The NATS JetStream implementation lives in nats_bus.hpp;
// MemoryBus is the in-process implementation used by unit tests and the
// backtester so the identical strategy/risk code runs in both.
//
// Semantics (both implementations):
//  * at-least-once delivery; consumers must dedupe on msg_id (see Dedupe).
//  * a handler that returns normally acks; a handler that throws naks and the
//    message is redelivered later.
//  * subject filters support NATS wildcards: '*' one token, '>' the rest.
#pragma once
#include "envelope.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace at {

struct Delivery {
    std::string subject;
    Envelope env;
    std::uint64_t seq = 0;      // stream sequence when known
    int redelivery_count = 0;
};

using Handler = std::function<void(const Delivery&)>;

class IBus {
public:
    virtual ~IBus() = default;
    virtual void publish(const std::string& subject, const Envelope& env) = 0;
    // durable: consumer name; a restarted service with the same durable resumes where it left off.
    virtual void subscribe(const std::string& subject_filter, const std::string& durable, Handler handler) = 0;
    // Dispatch pending deliveries on the calling thread. Returns number handled.
    virtual std::size_t poll() = 0;
    virtual void close() = 0;
};

bool subject_matches(const std::string& filter, const std::string& subject);

// Consumer-side idempotency guard: remembers the last `capacity` msg_ids.
class Dedupe {
public:
    explicit Dedupe(std::size_t capacity = 100000) : capacity_(capacity) {}
    // true the first time an id is seen, false on any repeat.
    bool first_time(const std::string& msg_id);
    std::size_t size() const { return seen_.size(); }
private:
    std::size_t capacity_;
    std::unordered_set<std::string> seen_;
    std::deque<std::string> order_;
};

class MemoryBus : public IBus {
public:
    void publish(const std::string& subject, const Envelope& env) override;
    void subscribe(const std::string& subject_filter, const std::string& durable, Handler handler) override;
    std::size_t poll() override;
    void close() override {}

    // Drain until no pending deliveries remain (handlers may publish). Returns total handled.
    std::size_t drain(std::size_t max_rounds = 1000);

    // Test hooks.
    void set_redeliver_every_message(bool on) { redeliver_ = on; }   // simulate at-least-once
    const std::vector<std::pair<std::string, Envelope>>& log() const { return log_; }
    std::vector<Envelope> published(const std::string& subject_filter) const;
    void clear_log() { log_.clear(); }

private:
    struct Sub { std::string filter; std::string durable; Handler handler; };
    struct Pending { std::size_t sub_index; Delivery delivery; };
    std::vector<Sub> subs_;
    std::deque<Pending> queue_;
    std::vector<std::pair<std::string, Envelope>> log_;
    std::uint64_t seq_ = 0;
    bool redeliver_ = false;
    std::mutex m_;
};

} // namespace at
