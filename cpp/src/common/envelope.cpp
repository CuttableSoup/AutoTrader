#include "envelope.hpp"

#include "time.hpp"
#include "uuid.hpp"

#include <stdexcept>

namespace at {

nlohmann::json Envelope::to_json() const {
    nlohmann::json j = {
        {"schema_version", schema_version},
        {"msg_id", msg_id},
        {"ts_utc", ts_utc},
        {"producer", producer},
        {"payload", payload},
    };
    if (correlation_id) j["correlation_id"] = *correlation_id;
    return j;
}

std::string Envelope::dump() const { return to_json().dump(); }

Envelope Envelope::from_json(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("envelope: not an object");
    Envelope e;
    for (const char* k : {"schema_version", "msg_id", "ts_utc", "producer"}) {
        if (!j.contains(k) || !j[k].is_string()) throw std::runtime_error(std::string("envelope: missing ") + k);
    }
    e.schema_version = j["schema_version"].get<std::string>();
    e.msg_id = j["msg_id"].get<std::string>();
    e.ts_utc = j["ts_utc"].get<std::string>();
    e.producer = j["producer"].get<std::string>();
    if (!is_uuid(e.msg_id)) throw std::runtime_error("envelope: msg_id is not a uuid");
    if (!parse_iso_utc(e.ts_utc)) throw std::runtime_error("envelope: ts_utc not ISO-8601 UTC");
    if (j.contains("correlation_id") && j["correlation_id"].is_string())
        e.correlation_id = j["correlation_id"].get<std::string>();
    if (!j.contains("payload") || !j["payload"].is_object()) throw std::runtime_error("envelope: payload missing");
    e.payload = j["payload"];
    return e;
}

Envelope Envelope::parse(std::string_view raw) {
    return from_json(nlohmann::json::parse(raw));
}

Envelope make_envelope(std::string producer, nlohmann::json payload, std::optional<std::string> correlation_id) {
    Envelope e;
    e.msg_id = uuid_v4();
    e.ts_utc = now_utc_iso();
    e.producer = std::move(producer);
    e.correlation_id = std::move(correlation_id);
    e.payload = std::move(payload);
    return e;
}

} // namespace at
