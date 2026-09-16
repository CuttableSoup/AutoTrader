#include "config.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace at {

Config::Config(nlohmann::json root, std::filesystem::path base_dir)
    : root_(std::move(root)), base_dir_(std::move(base_dir)) {}

Config Config::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("config: cannot open " + file.string());
    nlohmann::json j;
    try { in >> j; } catch (const std::exception& e) {
        throw std::runtime_error("config: parse error in " + file.string() + ": " + e.what());
    }
    auto base = std::filesystem::absolute(file).parent_path();
    // Config files live in <root>/config; resolve relative paths against <root>.
    if (auto root = find_project_root(base)) base = *root;
    return Config(std::move(j), base);
}

const nlohmann::json* Config::find(const std::string& dotted) const {
    const nlohmann::json* cur = &root_;
    std::string tok;
    std::istringstream ss(dotted);
    while (std::getline(ss, tok, '.')) {
        if (!cur->is_object() || !cur->contains(tok)) return nullptr;
        cur = &(*cur)[tok];
    }
    return cur;
}

std::filesystem::path Config::resolve(const std::string& p) const {
    std::filesystem::path fp(p);
    if (fp.is_absolute() || base_dir_.empty()) return fp;
    return base_dir_ / fp;
}

std::optional<std::filesystem::path> find_project_root(std::filesystem::path start, const std::string& marker) {
    std::error_code ec;
    start = std::filesystem::absolute(start, ec);
    for (auto p = start; !p.empty(); p = p.parent_path()) {
        if (std::filesystem::exists(p / marker, ec)) return p;
        if (p == p.root_path()) break;
    }
    return std::nullopt;
}

} // namespace at
