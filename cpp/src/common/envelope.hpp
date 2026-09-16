// Message envelope shared by every subject (schemas/v1/envelope.schema.json).
#pragma once
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace at {

struct Envelope {
    std::string schema_version = "1.0";
    std::string msg_id;                     // uuid v4
    std::string ts_utc;                     // ISO-8601 UTC with ms
    std::string producer;
    std::optional<std::string> correlation_id;
    nlohmann::json payload = nlohmann::json::object();

    nlohmann::json to_json() const;
    std::string dump() const;
    static Envelope from_json(const nlohmann::json& j); // throws std::runtime_error on missing/invalid fields
    static Envelope parse(std::string_view raw);        // throws
};

// New envelope with fresh msg_id and current timestamp.
Envelope make_envelope(std::string producer, nlohmann::json payload,
                       std::optional<std::string> correlation_id = std::nullopt);

} // namespace at
