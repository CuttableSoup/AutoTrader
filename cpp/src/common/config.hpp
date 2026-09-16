// JSON config files with dotted-path lookup and defaults.
#pragma once
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace at {

class Config {
public:
    Config() = default;
    explicit Config(nlohmann::json root, std::filesystem::path base_dir = {});
    static Config load(const std::filesystem::path& file);

    // "a.b.c" lookup. Missing -> default.
    template <typename T>
    T get(const std::string& dotted, const T& def) const {
        const nlohmann::json* n = find(dotted);
        if (!n || n->is_null()) return def;
        try { return n->get<T>(); } catch (...) { return def; }
    }
    template <typename T>
    std::optional<T> get_opt(const std::string& dotted) const {
        const nlohmann::json* n = find(dotted);
        if (!n || n->is_null()) return std::nullopt;
        try { return n->get<T>(); } catch (...) { return std::nullopt; }
    }
    // Throws std::runtime_error if missing.
    template <typename T>
    T require(const std::string& dotted) const {
        const nlohmann::json* n = find(dotted);
        if (!n || n->is_null()) throw std::runtime_error("config: missing required key '" + dotted + "'");
        return n->get<T>();
    }
    const nlohmann::json* find(const std::string& dotted) const;
    const nlohmann::json& root() const { return root_; }
    nlohmann::json& mutable_root() { return root_; }
    bool has(const std::string& dotted) const { return find(dotted) != nullptr; }

    // Resolve a path relative to the directory the config file was loaded from
    // (or the project root if given), unless it is already absolute.
    std::filesystem::path resolve(const std::string& p) const;
    const std::filesystem::path& base_dir() const { return base_dir_; }

private:
    nlohmann::json root_ = nlohmann::json::object();
    std::filesystem::path base_dir_;
};

// Walk upward from `start` until a directory containing `marker` is found.
std::optional<std::filesystem::path> find_project_root(std::filesystem::path start, const std::string& marker = "schemas/topics.json");

} // namespace at
