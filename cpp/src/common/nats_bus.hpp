// NATS JetStream implementation of IBus (cnats). Durable pull consumers with
// explicit ack; publishes carry Nats-Msg-Id = envelope.msg_id so JetStream
// dedupes redeliveries inside the stream's duplicate window.
#pragma once
#include "bus.hpp"
#include "schema.hpp"

#include <memory>
#include <string>

namespace at {

class NatsBus : public IBus {
public:
    // client_name shows up in nats-server monitoring; used as the consumer name prefix.
    NatsBus(const std::string& url, const std::string& client_name);
    ~NatsBus() override;

    // Create or update every stream in the registry (idempotent). Call once at startup.
    void ensure_streams(const SchemaRegistry& registry);

    void publish(const std::string& subject, const Envelope& env) override;
    void subscribe(const std::string& subject_filter, const std::string& durable, Handler handler) override;
    std::size_t poll() override;   // fetches up to 64 messages per subscription, 100 ms wait
    void close() override;

    bool connected() const;
    // Optional: validate every outgoing message against the registry before publishing.
    void set_validator(const SchemaRegistry* registry) { registry_ = registry; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const SchemaRegistry* registry_ = nullptr;
};

} // namespace at
