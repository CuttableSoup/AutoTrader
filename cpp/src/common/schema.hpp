// JSON Schema validation against schemas/ (envelope + per-subject payload).
#pragma once
#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace at {

class SchemaRegistry {
public:
    // schemas_dir contains topics.json and v1/*.schema.json.
    explicit SchemaRegistry(const std::filesystem::path& schemas_dir);
    ~SchemaRegistry();

    // Payload schema file (relative to schemas_dir) for a concrete subject, or "" if unregistered.
    std::string schema_for_subject(const std::string& subject) const;

    // Validate a full envelope JSON for a subject. Returns list of errors (empty = valid).
    std::vector<std::string> validate(const std::string& subject, const nlohmann::json& envelope_json) const;
    // Throws std::runtime_error with all errors joined.
    void validate_or_throw(const std::string& subject, const nlohmann::json& envelope_json) const;

    struct StreamSpec { std::string name; std::vector<std::string> subjects; int max_age_days; long long max_msgs; int duplicate_window_s; };
    const std::vector<StreamSpec>& streams() const { return streams_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<StreamSpec> streams_;
    std::vector<std::pair<std::string, std::string>> topic_filters_; // (subject filter, schema file)
};

} // namespace at
