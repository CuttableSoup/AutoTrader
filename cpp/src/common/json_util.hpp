// Small JSON helpers. json_obj/json_arr return references (never temporaries),
// so they are safe to iterate with .items() in a range-for.
#pragma once
#include <nlohmann/json.hpp>

namespace at {

inline const nlohmann::json& json_obj(const nlohmann::json& j, const char* key) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (j.is_object()) {
        auto it = j.find(key);
        if (it != j.end() && it->is_object()) return *it;
    }
    return empty;
}

inline const nlohmann::json& json_arr(const nlohmann::json& j, const char* key) {
    static const nlohmann::json empty = nlohmann::json::array();
    if (j.is_object()) {
        auto it = j.find(key);
        if (it != j.end() && it->is_array()) return *it;
    }
    return empty;
}

} // namespace at
