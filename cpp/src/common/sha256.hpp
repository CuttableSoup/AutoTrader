// Self-contained SHA-256 for deterministic identifiers (client_order_id,
// event_id). Keeps the strategy/risk core free of OpenSSL.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace at {

std::array<std::uint8_t, 32> sha256(std::string_view data);

// Lowercase hex, 64 chars.
std::string sha256_hex(std::string_view data);

} // namespace at
