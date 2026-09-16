// RFC 4122 version-4 UUIDs for msg_id. Thread-safe.
#pragma once
#include <string>
#include <string_view>

namespace at {

std::string uuid_v4();

// Lowercase 8-4-4-4-12 hex with a valid version/variant nibble.
bool is_uuid(std::string_view s);

} // namespace at
