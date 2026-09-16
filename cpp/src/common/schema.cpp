#include "schema.hpp"

#include "bus.hpp"

#include <nlohmann/json-schema.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace at {

namespace {
nlohmann::json read_json(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in) throw std::runtime_error("schema: cannot open " + p.string());
    nlohmann::json j;
    in >> j;
    return j;
}

class CollectingErrorHandler : public nlohmann::json_schema::basic_error_handler {
public:
    std::vector<std::string> errors;
    void error(const nlohmann::json::json_pointer& ptr, const nlohmann::json& instance,
               const std::string& message) override {
        nlohmann::json_schema::basic_error_handler::error(ptr, instance, message);
        errors.push_back(ptr.to_string() + ": " + message + " (got " + instance.dump().substr(0, 120) + ")");
    }
};
} // namespace

struct SchemaRegistry::Impl {
    std::filesystem::path dir;
    nlohmann::json_schema::json_validator envelope;
    std::map<std::string, std::unique_ptr<nlohmann::json_schema::json_validator>> payload; // by schema file
};

SchemaRegistry::SchemaRegistry(const std::filesystem::path& schemas_dir) : impl_(std::make_unique<Impl>()) {
    impl_->dir = schemas_dir;
    impl_->envelope.set_root_schema(read_json(schemas_dir / "v1" / "envelope.schema.json"));
    auto topics = read_json(schemas_dir / "topics.json");
    for (const auto& s : topics.at("streams")) {
        StreamSpec spec;
        spec.name = s.at("name").get<std::string>();
        for (const auto& subj : s.at("subjects")) spec.subjects.push_back(subj.get<std::string>());
        spec.max_age_days = s.at("max_age_days").get<int>();
        spec.max_msgs = s.at("max_msgs").get<long long>();
        spec.duplicate_window_s = s.value("duplicate_window_s", 120);
        streams_.push_back(std::move(spec));
    }
    for (const auto& t : topics.at("topics")) {
        std::string filter = t.at("subject").get<std::string>();
        std::string file = t.at("schema").get<std::string>();
        topic_filters_.emplace_back(filter, file);
        if (!impl_->payload.count(file)) {
            auto v = std::make_unique<nlohmann::json_schema::json_validator>();
            v->set_root_schema(read_json(schemas_dir / file));
            impl_->payload[file] = std::move(v);
        }
    }
}

SchemaRegistry::~SchemaRegistry() = default;

std::string SchemaRegistry::schema_for_subject(const std::string& subject) const {
    for (const auto& [filter, file] : topic_filters_)
        if (subject_matches(filter, subject)) return file;
    return "";
}

std::vector<std::string> SchemaRegistry::validate(const std::string& subject, const nlohmann::json& env) const {
    std::vector<std::string> errors;
    {
        CollectingErrorHandler h;
        impl_->envelope.validate(env, h);
        for (auto& e : h.errors) errors.push_back("envelope" + e);
    }
    std::string file = schema_for_subject(subject);
    if (file.empty()) { errors.push_back("subject '" + subject + "' is not registered in topics.json"); return errors; }
    if (env.contains("payload")) {
        CollectingErrorHandler h;
        impl_->payload.at(file)->validate(env["payload"], h);
        for (auto& e : h.errors) errors.push_back("payload" + e);
    }
    return errors;
}

void SchemaRegistry::validate_or_throw(const std::string& subject, const nlohmann::json& env) const {
    auto errs = validate(subject, env);
    if (errs.empty()) return;
    std::ostringstream ss;
    ss << "schema validation failed for " << subject << ":";
    for (auto& e : errs) ss << "\n  " << e;
    throw std::runtime_error(ss.str());
}

} // namespace at
